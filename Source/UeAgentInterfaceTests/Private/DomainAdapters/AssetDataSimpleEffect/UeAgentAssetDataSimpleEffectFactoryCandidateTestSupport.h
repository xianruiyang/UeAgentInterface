#pragma once

#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::Tests::AssetDataSimpleEffect
{
	using namespace Contract;
	using namespace Discovery;
	using namespace DomainAdapters::AssetDataSimpleEffect;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	class FFixtureFactoryCandidateResolver final
		: public IUeAgentAssetDataSimpleEffectFactoryCandidateResolver
	{
	public:
		explicit FFixtureFactoryCandidateResolver(
			TSharedPtr<FJsonObject> InFixture)
			: Fixture(MoveTemp(InFixture))
		{
		}

		bool TryResolveForPlanning(
			const FString& FactoryRef,
			const FUeAgentCatalogRevision& ExpectedRevision,
			const FString& ProfileId,
			FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding,
			FExecutionMutationError& OutError) override
		{
			const bool bInvalidDataObject =
				FactoryRef
					== TEXT(
						"uai://factory/asset-data/"
						"data-object/invalid-uobject");
			const TSharedPtr<FJsonObject> Contract =
				bInvalidDataObject
					? FindByProfile(TEXT("data_object"))
					: FindByPublicRef(FactoryRef);
			return TryBuild(
				Contract,
				ExpectedRevision,
				ProfileId,
				bInvalidDataObject
					? UObject::StaticClass()
					: nullptr,
				bInvalidDataObject
					? FactoryRef
					: FString(),
				bInvalidDataObject
					? TEXT(
						"uai.asset_data.factory_binding:"
						"data-object:invalid-uobject:v1")
					: FString(),
				OutBinding,
				OutError);
		}

		bool TryResolveFrozenBinding(
			const FString& FactoryBindingId,
			const FUeAgentCatalogRevision& ExpectedRevision,
			const FString& ProfileId,
			FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding,
			FExecutionMutationError& OutError) override
		{
			return TryBuild(
				FindByBindingId(FactoryBindingId),
				ExpectedRevision,
				ProfileId,
				nullptr,
				FString(),
				FString(),
				OutBinding,
				OutError);
		}

	private:
		TSharedPtr<FJsonObject> FindByPublicRef(
			const FString& PublicRef) const
		{
			return Find(
				TEXT("candidate_ref"),
				PublicRef);
		}

		TSharedPtr<FJsonObject> FindByBindingId(
			const FString& BindingId) const
		{
			return Find(
				TEXT("factory_binding_id"),
				BindingId);
		}

		TSharedPtr<FJsonObject> FindByProfile(
			const FString& ProfileId) const
		{
			return Find(TEXT("profile_id"), ProfileId);
		}

		TSharedPtr<FJsonObject> Find(
			const FString& Field,
			const FString& Value) const
		{
			if (!Fixture.IsValid())
			{
				return nullptr;
			}
			for (const TSharedPtr<FJsonValue>& Item
				: Fixture->GetArrayField(TEXT("factory_contracts")))
			{
				const TSharedPtr<FJsonObject> Contract =
					Item->AsObject();
				if (Contract->GetStringField(Field) == Value)
				{
					return Contract;
				}
			}
			return nullptr;
		}

		bool TryBuild(
			const TSharedPtr<FJsonObject>& Contract,
			const FUeAgentCatalogRevision& ExpectedRevision,
			const FString& ProfileId,
			UClass* OverrideClass,
			const FString& OverridePublicRef,
			const FString& OverrideBindingId,
			FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding,
			FExecutionMutationError& OutError) const
		{
			OutBinding =
				FUeAgentAssetDataSimpleEffectFactoryBinding();
			OutError = FExecutionMutationError();
			if (!Contract.IsValid())
			{
				OutError = FExecutionMutationError(
					TEXT("asset_data_factory_candidate_not_found"),
					ProfileId);
				return false;
			}

			const FUeAgentCatalogRevision ActualRevision(
				Contract->GetStringField(
					TEXT("catalog_source_id")),
				Contract->GetStringField(
					TEXT("catalog_generation")),
				static_cast<int64>(
					Contract->GetNumberField(
						TEXT("catalog_sequence"))));
			if (ActualRevision != ExpectedRevision)
			{
				OutError = FExecutionMutationError(
					TEXT("asset_data_factory_candidate_stale"),
					ProfileId);
				return false;
			}

			const FString PublicRef =
				OverridePublicRef.IsEmpty()
					? Contract->GetStringField(
						TEXT("candidate_ref"))
					: OverridePublicRef;
			const FString BindingId =
				OverrideBindingId.IsEmpty()
					? Contract->GetStringField(
						TEXT("factory_binding_id"))
					: OverrideBindingId;
			UClass* ConcreteClass = OverrideClass;
			if (!ConcreteClass)
			{
				const FString ClassPath =
					Contract->GetStringField(
						TEXT("asset_class_path"));
				ConcreteClass =
					FindObject<UClass>(nullptr, *ClassPath);
				if (!ConcreteClass)
				{
					ConcreteClass =
						LoadObject<UClass>(nullptr, *ClassPath);
				}
			}
			const FString CandidateHash =
				TEXT(
					"sha256:11111111111111111111111111111111"
					"11111111111111111111111111111111");
			OutBinding =
				FUeAgentAssetDataSimpleEffectFactoryBinding(
					FUeAgentCandidateBinding(
						BindingId,
						FUeAgentCandidateSummary(
							FUeAgentCanonicalIdentity(
								TEXT("uai.asset_data.factory:")
									+ PublicRef,
								TEXT("factory_candidate"),
								TEXT("asset-data-factory-catalog"),
								PublicRef,
								TEXT("asset_data_factory"),
								ActualRevision.GetGeneration()),
							FUeAgentDescriptorIdentity(
								Contract->GetStringField(
									TEXT("factory_contract_id")),
								1,
								CandidateHash),
							TEXT("asset_data_factory"),
							Contract->GetStringField(
								TEXT("asset_class")),
							Contract->GetStringField(
								TEXT("class_constraint_id")),
							{
								TEXT("Asset"),
								TEXT("Factory"),
							}),
						ActualRevision,
						PublicRef,
						TEXT("asset-data-factory-candidate-read"),
						{EUeAgentCandidateOperation::Create},
						Contract->GetStringField(
							TEXT("class_constraint_id"))),
					Contract->GetStringField(TEXT("profile_id")),
					ConcreteClass,
					Contract->GetStringField(
						TEXT("factory_mechanic_id")));
			if (!OutBinding.IsValid())
			{
				OutError = FExecutionMutationError(
					TEXT("asset_data_factory_candidate_invalid"),
					ProfileId);
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonObject> Fixture;
	};
}
