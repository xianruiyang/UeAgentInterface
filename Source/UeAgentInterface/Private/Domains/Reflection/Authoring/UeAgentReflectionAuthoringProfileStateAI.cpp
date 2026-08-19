// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileStateAI.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentReflectionAuthoring::ProfileStateAI
{
	namespace
	{
		constexpr const TCHAR* BehaviorTreeTypeId = TEXT("behavior_tree");
		constexpr const TCHAR* EqsTypeId = TEXT("eqs");
		constexpr const TCHAR* UnsupportedError = TEXT("reflection_profile_state_ai_unsupported");

		struct FBehaviorTreeRootSpec
		{
			bool bClear = false;
			UClass* RootClass = nullptr;
			FString Name;
		};

		struct FEqsOptionSpec
		{
			UClass* GeneratorClass = nullptr;
		};

		struct FEqsOptionPlan
		{
			UEnvQueryOption* Option = nullptr;
			UEnvQueryGenerator* Generator = nullptr;
			bool bReplaceGenerator = false;
		};

		bool IsConcreteChildClass(UClass* Class, const UClass* BaseClass)
		{
			return Class
				&& Class->IsChildOf(BaseClass)
				&& !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
		}

		UClass* ResolveConcreteClass(
			const FString& ClassPath,
			const UClass* BaseClass)
		{
			const FString NormalizedPath = ClassPath.TrimStartAndEnd();
			if (NormalizedPath.IsEmpty()) return nullptr;
			UClass* Class = FSoftClassPath(NormalizedPath).TryLoadClass<UObject>();
			return IsConcreteChildClass(Class, BaseClass) ? Class : nullptr;
		}

		bool ParseBehaviorTreeRoot(
			const TSharedPtr<FJsonValue>& Value,
			FBehaviorTreeRootSpec& OutSpec,
			FString& OutError)
		{
			OutSpec = FBehaviorTreeRootSpec();
			if (!Value.IsValid())
			{
				OutError = TEXT("behavior_tree_root_missing");
				return false;
			}
			if (Value->Type == EJson::Null)
			{
				OutSpec.bClear = true;
				OutError.Reset();
				return true;
			}
			if (Value->Type != EJson::Object)
			{
				OutError = TEXT("behavior_tree_root_invalid");
				return false;
			}

			const TSharedPtr<FJsonObject> Root = Value->AsObject();
			FString ClassPath;
			if (!Root.IsValid()
				|| !Root->TryGetStringField(TEXT("class_path"), ClassPath)
				|| !Root->TryGetStringField(TEXT("name"), OutSpec.Name))
			{
				OutError = TEXT("behavior_tree_root_missing");
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
			{
				if (Pair.Key != TEXT("class_path") && Pair.Key != TEXT("name"))
				{
					OutError = TEXT("behavior_tree_root_unknown_field:") + Pair.Key;
					return false;
				}
			}
			OutSpec.RootClass = ResolveConcreteClass(ClassPath, UBTCompositeNode::StaticClass());
			if (!OutSpec.RootClass)
			{
				OutError = TEXT("behavior_tree_root_class_invalid");
				return false;
			}
			OutError.Reset();
			return true;
		}

		bool ParseEqsOptions(
			const TSharedPtr<FJsonValue>& Value,
			TArray<FEqsOptionSpec>& OutSpecs,
			FString& OutError)
		{
			OutSpecs.Reset();
			if (!Value.IsValid() || Value->Type != EJson::Array)
			{
				OutError = TEXT("eqs_options_invalid");
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>& Options = Value->AsArray();
			OutSpecs.Reserve(Options.Num());
			for (int32 Index = 0; Index < Options.Num(); ++Index)
			{
				if (!Options[Index].IsValid() || Options[Index]->Type != EJson::Object)
				{
					OutError = TEXT("eqs_option_invalid");
					return false;
				}
				const TSharedPtr<FJsonObject> Option = Options[Index]->AsObject();
				FString Id;
				FString GeneratorClassPath;
				if (!Option.IsValid()
					|| !Option->TryGetStringField(TEXT("id"), Id)
					|| Id != FString::Printf(TEXT("option_%d"), Index))
				{
					OutError = TEXT("eqs_option_id_invalid");
					return false;
				}
				if (!Option->TryGetStringField(TEXT("generator_class"), GeneratorClassPath))
				{
					OutError = TEXT("eqs_generator_missing");
					return false;
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Option->Values)
				{
					if (Pair.Key != TEXT("id") && Pair.Key != TEXT("generator_class"))
					{
						OutError = TEXT("eqs_option_unknown_field:") + Pair.Key;
						return false;
					}
				}
				FEqsOptionSpec& Spec = OutSpecs.AddDefaulted_GetRef();
				Spec.GeneratorClass = ResolveConcreteClass(
					GeneratorClassPath,
					UEnvQueryGenerator::StaticClass());
				if (!Spec.GeneratorClass)
				{
					OutError = TEXT("eqs_generator_class_invalid");
					return false;
				}
			}
			OutError.Reset();
			return true;
		}

		void NotifyAssetChanged(UObject& Asset)
		{
			Asset.MarkPackageDirty();
#if WITH_EDITOR
			Asset.PostEditChange();
#endif
		}

		bool ReadBehaviorTree(
			UObject& Asset,
			TSharedPtr<FJsonValue>& OutValue,
			FString& OutError)
		{
			const UBehaviorTree* BehaviorTree = Cast<UBehaviorTree>(&Asset);
			if (!BehaviorTree)
			{
				OutError = TEXT("behavior_tree_asset_invalid");
				return false;
			}
			if (!BehaviorTree->RootNode)
			{
				OutValue = MakeShared<FJsonValueNull>();
				OutError.Reset();
				return true;
			}
			if (BehaviorTree->RootNode->GetOuter() != BehaviorTree)
			{
				OutError = TEXT("behavior_tree_root_outer_invalid");
				return false;
			}

			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("class_path"), BehaviorTree->RootNode->GetClass()->GetPathName());
			Root->SetStringField(TEXT("name"), BehaviorTree->RootNode->NodeName);
			OutValue = MakeShared<FJsonValueObject>(Root);
			OutError.Reset();
			return true;
		}

		bool ReadEqs(
			UObject& Asset,
			TSharedPtr<FJsonValue>& OutValue,
			FString& OutError)
		{
			const UEnvQuery* Query = Cast<UEnvQuery>(&Asset);
			if (!Query)
			{
				OutError = TEXT("eqs_asset_invalid");
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> Options;
			const TArray<UEnvQueryOption*>& QueryOptions = Query->GetOptions();
			Options.Reserve(QueryOptions.Num());
			for (int32 Index = 0; Index < QueryOptions.Num(); ++Index)
			{
				const UEnvQueryOption* Option = QueryOptions[Index];
				const UEnvQueryGenerator* Generator = Option ? Option->Generator.Get() : nullptr;
				if (!Option || !Generator)
				{
					OutError = TEXT("eqs_generator_missing");
					return false;
				}
				if (Option->GetOuter() != Query || Generator->GetOuter() != Option)
				{
					OutError = TEXT("eqs_option_outer_invalid");
					return false;
				}
				TSharedRef<FJsonObject> OptionValue = MakeShared<FJsonObject>();
				OptionValue->SetStringField(TEXT("id"), FString::Printf(TEXT("option_%d"), Index));
				OptionValue->SetStringField(TEXT("generator_class"), Generator->GetClass()->GetPathName());
				Options.Add(MakeShared<FJsonValueObject>(OptionValue));
			}
			OutValue = MakeShared<FJsonValueArray>(MoveTemp(Options));
			OutError.Reset();
			return true;
		}

		bool ApplyBehaviorTree(
			UObject& Asset,
			const TSharedPtr<FJsonValue>& Value,
			FString& OutError)
		{
			UBehaviorTree* BehaviorTree = Cast<UBehaviorTree>(&Asset);
			if (!BehaviorTree)
			{
				OutError = TEXT("behavior_tree_asset_invalid");
				return false;
			}
			FBehaviorTreeRootSpec Spec;
			if (!ParseBehaviorTreeRoot(Value, Spec, OutError)) return false;

			UBTCompositeNode* ExistingRoot = BehaviorTree->RootNode;
			if (ExistingRoot && ExistingRoot->GetOuter() != BehaviorTree)
			{
				OutError = TEXT("behavior_tree_root_outer_invalid");
				return false;
			}
			if (Spec.bClear)
			{
				if (!ExistingRoot)
				{
					OutError.Reset();
					return true;
				}
				if (!ExistingRoot->Children.IsEmpty() || !ExistingRoot->Services.IsEmpty())
				{
					OutError = TEXT("behavior_tree_root_replace_unsafe");
					return false;
				}
				BehaviorTree->Modify();
				BehaviorTree->RootNode = nullptr;
				NotifyAssetChanged(*BehaviorTree);
				OutError.Reset();
				return true;
			}

			if (ExistingRoot
				&& ExistingRoot->GetClass() == Spec.RootClass
				&& ExistingRoot->NodeName == Spec.Name)
			{
				OutError.Reset();
				return true;
			}
			if (ExistingRoot
				&& ExistingRoot->GetClass() != Spec.RootClass
				&& (!ExistingRoot->Children.IsEmpty() || !ExistingRoot->Services.IsEmpty()))
			{
				OutError = TEXT("behavior_tree_root_replace_unsafe");
				return false;
			}

			if (ExistingRoot && ExistingRoot->GetClass() == Spec.RootClass)
			{
				BehaviorTree->Modify();
				ExistingRoot->Modify();
				ExistingRoot->NodeName = Spec.Name;
				NotifyAssetChanged(*BehaviorTree);
				OutError.Reset();
				return true;
			}

			UBTCompositeNode* NewRoot = NewObject<UBTCompositeNode>(
				BehaviorTree,
				Spec.RootClass,
				NAME_None,
				RF_Transactional);
			if (!NewRoot)
			{
				OutError = TEXT("behavior_tree_root_create_failed");
				return false;
			}
			NewRoot->NodeName = Spec.Name;
			BehaviorTree->Modify();
			BehaviorTree->RootNode = NewRoot;
			NotifyAssetChanged(*BehaviorTree);
			OutError.Reset();
			return true;
		}

		bool ApplyEqs(
			UObject& Asset,
			const TSharedPtr<FJsonValue>& Value,
			FString& OutError)
		{
			UEnvQuery* Query = Cast<UEnvQuery>(&Asset);
			if (!Query)
			{
				OutError = TEXT("eqs_asset_invalid");
				return false;
			}
			TArray<FEqsOptionSpec> Specs;
			if (!ParseEqsOptions(Value, Specs, OutError)) return false;

			TArray<TObjectPtr<UEnvQueryOption>>& ExistingOptions = Query->GetOptionsMutable();
			for (int32 Index = 0; Index < ExistingOptions.Num(); ++Index)
			{
				const UEnvQueryOption* Option = ExistingOptions[Index];
				if (Option && Option->GetOuter() != Query)
				{
					OutError = TEXT("eqs_option_outer_invalid");
					return false;
				}
				if (Option && Option->Generator && Option->Generator->GetOuter() != Option)
				{
					OutError = TEXT("eqs_generator_outer_invalid");
					return false;
				}
				if (Index >= Specs.Num() && Option && !Option->Tests.IsEmpty())
				{
					OutError = TEXT("eqs_option_replace_unsafe");
					return false;
				}
			}

			TArray<FEqsOptionPlan> Plans;
			Plans.Reserve(Specs.Num());
			bool bChanged = ExistingOptions.Num() != Specs.Num();
			for (int32 Index = 0; Index < Specs.Num(); ++Index)
			{
				FEqsOptionPlan& Plan = Plans.AddDefaulted_GetRef();
				Plan.Option = ExistingOptions.IsValidIndex(Index) ? ExistingOptions[Index].Get() : nullptr;
				if (!Plan.Option)
				{
					Plan.Option = NewObject<UEnvQueryOption>(
						Query,
						UEnvQueryOption::StaticClass(),
						NAME_None,
						RF_Transactional);
					if (!Plan.Option)
					{
						OutError = TEXT("eqs_option_create_failed");
						return false;
					}
					bChanged = true;
				}

				UEnvQueryGenerator* ExistingGenerator = Plan.Option->Generator.Get();
				if (!ExistingGenerator || ExistingGenerator->GetClass() != Specs[Index].GeneratorClass)
				{
					Plan.Generator = NewObject<UEnvQueryGenerator>(
						Plan.Option,
						Specs[Index].GeneratorClass,
						NAME_None,
						RF_Transactional);
					if (!Plan.Generator)
					{
						OutError = TEXT("eqs_generator_create_failed");
						return false;
					}
					Plan.bReplaceGenerator = true;
					bChanged = true;
				}
				else
				{
					Plan.Generator = ExistingGenerator;
				}
			}

			if (!bChanged)
			{
				OutError.Reset();
				return true;
			}

			Query->Modify();
			TArray<TObjectPtr<UEnvQueryOption>> NewOptions;
			NewOptions.Reserve(Plans.Num());
			for (const FEqsOptionPlan& Plan : Plans)
			{
				if (Plan.bReplaceGenerator)
				{
					Plan.Option->Modify();
					Plan.Option->Generator = Plan.Generator;
				}
				NewOptions.Add(Plan.Option);
			}
			ExistingOptions = MoveTemp(NewOptions);
			NotifyAssetChanged(*Query);
			OutError.Reset();
			return true;
		}
	}

	bool Supports(const FProfile& Profile)
	{
		return Profile.TypeId == BehaviorTreeTypeId || Profile.TypeId == EqsTypeId;
	}

	bool Validate(
		const FProfile& Profile,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutError)
	{
		if (Profile.TypeId == BehaviorTreeTypeId)
		{
			FBehaviorTreeRootSpec Spec;
			return ParseBehaviorTreeRoot(Value, Spec, OutError);
		}
		if (Profile.TypeId == EqsTypeId)
		{
			TArray<FEqsOptionSpec> Specs;
			return ParseEqsOptions(Value, Specs, OutError);
		}
		OutError = UnsupportedError;
		return false;
	}

	bool Read(
		const FProfile& Profile,
		UObject& Asset,
		TSharedPtr<FJsonValue>& OutValue,
		FString& OutError)
	{
		OutValue.Reset();
		if (Profile.TypeId == BehaviorTreeTypeId) return ReadBehaviorTree(Asset, OutValue, OutError);
		if (Profile.TypeId == EqsTypeId) return ReadEqs(Asset, OutValue, OutError);
		OutError = UnsupportedError;
		return false;
	}

	bool Apply(
		const FProfile& Profile,
		UObject& Asset,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutError)
	{
		if (Profile.TypeId == BehaviorTreeTypeId) return ApplyBehaviorTree(Asset, Value, OutError);
		if (Profile.TypeId == EqsTypeId) return ApplyEqs(Asset, Value, OutError);
		OutError = UnsupportedError;
		return false;
	}
}
