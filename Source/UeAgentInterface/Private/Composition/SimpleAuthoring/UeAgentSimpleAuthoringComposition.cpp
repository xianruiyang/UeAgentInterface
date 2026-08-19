#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"

#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"
#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	using namespace CommandContract;
	using namespace Contract;

	namespace
	{
		using FDescriptorRegistryFactory = bool (*)(
			TSharedPtr<const FUeAgentDescriptorRegistry>&,
			FString&);

		struct FFamilySource
		{
			const TCHAR* FamilyId;
			int32 ExpectedCount;
			FDescriptorRegistryFactory Factory;
		};

		const FFamilySource FamilySources[] = {
			{
				TEXT("world"),
				11,
				&DomainAdapters::WorldSimpleEffect::
					TryCreateWorldSimpleEffectDescriptorRegistry,
			},
			{
				TEXT("landscape"),
				2,
				&DomainAdapters::LandscapeGeometryEffect::
					TryCreateLandscapeGeometryEffectDescriptorRegistry,
			},
			{
				TEXT("mesh"),
				31,
				&DomainAdapters::ModelingGeometryEffect::
					TryCreateModelingGeometryEffectDescriptorRegistry,
			},
			{
				TEXT("asset_data"),
				53,
				&DomainAdapters::AssetDataSimpleEffect::
					TryCreateAssetDataSimpleEffectDescriptorRegistry,
			},
			{
				TEXT("editor_project"),
				7,
				&DomainAdapters::EditorProjectSimpleEffect::
					TryCreateEditorProjectSimpleEffectDescriptorRegistry,
			},
		};

		bool IsIdentifier(const FString& Value)
		{
			if (Value.IsEmpty()
				|| !(FChar::IsAlpha(Value[0])
					|| Value[0] == TEXT('_')))
			{
				return false;
			}
			for (int32 Index = 1; Index < Value.Len(); ++Index)
			{
				if (!(FChar::IsAlnum(Value[Index])
					|| Value[Index] == TEXT('_')))
				{
					return false;
				}
			}
			return true;
		}

		bool TryPythonBinding(
			const FString& CapabilityId,
			FString& OutNamespace,
			FString& OutMethod)
		{
			OutNamespace.Reset();
			OutMethod.Reset();
			TArray<FString> Segments;
			CapabilityId.ParseIntoArray(
				Segments,
				TEXT("."),
				true);
			if (Segments.Num() < 2)
			{
				return false;
			}
			OutNamespace = Segments[0];
			Segments.RemoveAt(0);
			OutMethod = FString::Join(Segments, TEXT("_"));
			return IsIdentifier(OutNamespace)
				&& IsIdentifier(OutMethod);
		}

		bool TryCollectDescriptors(
			TArray<FUeAgentOperationDescriptor>& OutDescriptors,
			FString& OutError)
		{
			OutDescriptors.Reset();
			OutError.Reset();
			for (const FFamilySource& Source : FamilySources)
			{
				TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
				FString FamilyError;
				if (!Source.Factory(Registry, FamilyError)
					|| !Registry.IsValid())
				{
					OutError = FString::Printf(
						TEXT(
							"simple_authoring_family_registry_failed:"
							"%s:%s"),
						Source.FamilyId,
						*FamilyError);
					return false;
				}
				if (Registry->GetDescriptors().Num()
					!= Source.ExpectedCount)
				{
					OutError = FString::Printf(
						TEXT(
							"simple_authoring_family_count_mismatch:"
							"%s:%d:%d"),
						Source.FamilyId,
						Registry->GetDescriptors().Num(),
						Source.ExpectedCount);
					return false;
				}
				for (const FUeAgentOperationDescriptor& Descriptor :
					Registry->GetDescriptors())
				{
					if (Descriptor.GetDomainId() != Source.FamilyId
						|| Descriptor.GetOperationKind()
							!= EUeAgentDescriptorOperationKind::
								Operation)
					{
						OutError = FString::Printf(
							TEXT(
								"simple_authoring_family_descriptor_"
								"mismatch:%s:%s"),
							Source.FamilyId,
							*Descriptor.GetIdentity()
								.GetDescriptorId());
						return false;
					}
					OutDescriptors.Add(Descriptor);
				}
			}
			if (OutDescriptors.Num() != 104)
			{
				OutError =
					TEXT("simple_authoring_descriptor_count_mismatch");
				return false;
			}
			return true;
		}

		bool TryParseObject(
			const FUeAgentContractValue& Value,
			TSharedPtr<FJsonObject>& OutObject)
		{
			OutObject.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Value.GetJsonValue());
			return FJsonSerializer::Deserialize(Reader, OutObject)
				&& OutObject.IsValid();
		}

		TSet<FString> RequiredFields(
			const TSharedPtr<FJsonObject>& Object)
		{
			TSet<FString> Result;
			if (!Object.IsValid())
			{
				return Result;
			}
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(TEXT("required"), Values)
				|| !Values)
			{
				return Result;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Name;
				if (Value.IsValid() && Value->TryGetString(Name))
				{
					Result.Add(Name);
				}
			}
			return Result;
		}

		void AddRootVariants(
			const TSharedPtr<FJsonObject>& Schema,
			TArray<TSharedPtr<FJsonObject>>& OutVariants)
		{
			if (!Schema.IsValid())
			{
				return;
			}
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Schema->TryGetObjectField(
				TEXT("properties"),
				Properties)
				&& Properties
				&& Properties->IsValid())
			{
				OutVariants.Add(Schema);
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* OneOf = nullptr;
			if (!Schema->TryGetArrayField(TEXT("oneOf"), OneOf)
				|| !OneOf)
			{
				return;
			}
			for (const TSharedPtr<FJsonValue>& Value : *OneOf)
			{
				const TSharedPtr<FJsonObject>* Variant = nullptr;
				if (Value.IsValid()
					&& Value->TryGetObject(Variant)
					&& Variant
					&& Variant->IsValid())
				{
					OutVariants.Add(*Variant);
				}
			}
		}

		EUeAgentCommandWireType WireTypeFromJson(
			const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid())
			{
				return EUeAgentCommandWireType::Any;
			}
			switch (Value->Type)
			{
			case EJson::Boolean:
				return EUeAgentCommandWireType::Boolean;
			case EJson::Number:
				return EUeAgentCommandWireType::Number;
			case EJson::String:
				return EUeAgentCommandWireType::String;
			case EJson::Array:
				return EUeAgentCommandWireType::Array;
			case EJson::Object:
				return EUeAgentCommandWireType::Object;
			default:
				return EUeAgentCommandWireType::Any;
			}
		}

		EUeAgentCommandWireType WireTypeFromName(
			const FString& TypeName)
		{
			if (TypeName == TEXT("boolean"))
			{
				return EUeAgentCommandWireType::Boolean;
			}
			if (TypeName == TEXT("integer"))
			{
				return EUeAgentCommandWireType::Integer;
			}
			if (TypeName == TEXT("number"))
			{
				return EUeAgentCommandWireType::Number;
			}
			if (TypeName == TEXT("string"))
			{
				return EUeAgentCommandWireType::String;
			}
			if (TypeName == TEXT("object"))
			{
				return EUeAgentCommandWireType::Object;
			}
			if (TypeName == TEXT("array"))
			{
				return EUeAgentCommandWireType::Array;
			}
			return EUeAgentCommandWireType::Any;
		}

		bool TrySchemaWireType(
			const TSharedPtr<FJsonObject>& Schema,
			EUeAgentCommandWireType& OutType,
			bool& bOutNullable)
		{
			OutType = EUeAgentCommandWireType::Any;
			bOutNullable = false;
			if (!Schema.IsValid())
			{
				return false;
			}

			FString TypeName;
			if (Schema->TryGetStringField(TEXT("type"), TypeName))
			{
				if (TypeName == TEXT("null"))
				{
					bOutNullable = true;
					return true;
				}
				OutType = WireTypeFromName(TypeName);
				return true;
			}

			const TSharedPtr<FJsonValue> ConstValue =
				Schema->TryGetField(TEXT("const"));
			if (ConstValue.IsValid())
			{
				OutType = WireTypeFromJson(ConstValue);
				bOutNullable =
					ConstValue->Type == EJson::Null;
				return true;
			}

			const TArray<TSharedPtr<FJsonValue>>* OneOf = nullptr;
			if (!Schema->TryGetArrayField(TEXT("oneOf"), OneOf)
				|| !OneOf
				|| OneOf->IsEmpty())
			{
				return true;
			}
			TOptional<EUeAgentCommandWireType> CommonType;
			for (const TSharedPtr<FJsonValue>& Value : *OneOf)
			{
				const TSharedPtr<FJsonObject>* Variant = nullptr;
				if (!Value.IsValid()
					|| !Value->TryGetObject(Variant)
					|| !Variant
					|| !Variant->IsValid())
				{
					CommonType =
						EUeAgentCommandWireType::Any;
					continue;
				}
				EUeAgentCommandWireType VariantType =
					EUeAgentCommandWireType::Any;
				bool bVariantNullable = false;
				TrySchemaWireType(
					*Variant,
					VariantType,
					bVariantNullable);
				bOutNullable = bOutNullable || bVariantNullable;
				if (VariantType == EUeAgentCommandWireType::Any
					&& bVariantNullable)
				{
					continue;
				}
				if (!CommonType.IsSet())
				{
					CommonType = VariantType;
				}
				else if (CommonType.GetValue() != VariantType)
				{
					CommonType =
						EUeAgentCommandWireType::Any;
				}
			}
			OutType = CommonType.Get(
				EUeAgentCommandWireType::Any);
			return true;
		}

		bool TryProjectParameters(
			const FUeAgentOperationDescriptor& Descriptor,
			TArray<FUeAgentCommandParameter>& OutParameters,
			FString& OutError)
		{
			OutParameters.Reset();
			TSharedPtr<FJsonObject> Root;
			if (!TryParseObject(Descriptor.GetInputSchema(), Root))
			{
				OutError =
					TEXT("simple_authoring_input_schema_parse_failed:")
					+ Descriptor.GetIdentity().GetDescriptorId();
				return false;
			}

			TArray<TSharedPtr<FJsonObject>> Variants;
			AddRootVariants(Root, Variants);
			if (Variants.IsEmpty())
			{
				OutError =
					TEXT(
						"simple_authoring_input_schema_root_unsupported:")
					+ Descriptor.GetIdentity().GetDescriptorId();
				return false;
			}

			TMap<FString, TArray<TSharedPtr<FJsonObject>>>
				SchemasByName;
			TMap<FString, int32> RequiredCountByName;
			for (const TSharedPtr<FJsonObject>& Variant : Variants)
			{
				const TSharedPtr<FJsonObject>* Properties = nullptr;
				if (!Variant->TryGetObjectField(
						TEXT("properties"),
						Properties)
					|| !Properties
					|| !Properties->IsValid())
				{
					OutError =
						TEXT(
							"simple_authoring_input_variant_invalid:")
						+ Descriptor.GetIdentity().GetDescriptorId();
					return false;
				}
				const TSet<FString> Required =
					RequiredFields(Variant);
				for (const TPair<
					FString,
					TSharedPtr<FJsonValue>>& Pair :
					(*Properties)->Values)
				{
					const TSharedPtr<FJsonObject>* PropertySchema =
						nullptr;
					if (!Pair.Value.IsValid()
						|| !Pair.Value->TryGetObject(
							PropertySchema)
						|| !PropertySchema
						|| !PropertySchema->IsValid())
					{
						OutError =
							TEXT(
								"simple_authoring_property_schema_"
								"invalid:")
							+ Descriptor.GetIdentity()
								.GetDescriptorId()
							+ TEXT(":")
							+ Pair.Key;
						return false;
					}
					SchemasByName.FindOrAdd(Pair.Key).Add(
						*PropertySchema);
					if (Required.Contains(Pair.Key))
					{
						RequiredCountByName.FindOrAdd(Pair.Key)
							+= 1;
					}
				}
			}

			TArray<FString> Names;
			SchemasByName.GetKeys(Names);
			Names.Sort();
			for (const FString& Name : Names)
			{
				EUeAgentCommandWireType WireType =
					EUeAgentCommandWireType::Any;
				bool bNullable = false;
				bool bFirst = true;
				for (const TSharedPtr<FJsonObject>& Schema :
					SchemasByName.FindChecked(Name))
				{
					EUeAgentCommandWireType VariantType =
						EUeAgentCommandWireType::Any;
					bool bVariantNullable = false;
					TrySchemaWireType(
						Schema,
						VariantType,
						bVariantNullable);
					bNullable = bNullable || bVariantNullable;
					if (bFirst)
					{
						WireType = VariantType;
						bFirst = false;
					}
					else if (WireType != VariantType)
					{
						WireType =
							EUeAgentCommandWireType::Any;
					}
				}
				const bool bRequired =
					SchemasByName.FindChecked(Name).Num()
						== Variants.Num()
					&& RequiredCountByName.FindRef(Name)
						== Variants.Num();
				OutParameters.Emplace(
					Name,
					WireType,
					bRequired,
					bNullable);
			}
			return true;
		}

		bool TryBuildRoutes(
			const FUeAgentDescriptorRegistry& Registry,
			TArray<FUeAgentSimpleAuthoringCommandRoute>& OutRoutes,
			FString& OutError)
		{
			OutRoutes.Reset();
			TSet<FString> CommandIds;
			TSet<FString> DescriptorIds;
			TSet<FString> PythonBindings;
			for (const FUeAgentOperationDescriptor& Descriptor :
				Registry.GetDescriptors())
			{
				FString Namespace;
				FString Method;
				if (!TryPythonBinding(
						Descriptor.GetCapabilityId(),
						Namespace,
						Method))
				{
					OutError =
						TEXT(
							"simple_authoring_route_invalid:")
						+ Descriptor.GetIdentity()
							.GetDescriptorId();
					return false;
				}
				FUeAgentSimpleAuthoringCommandRoute Route(
					Descriptor.GetCapabilityId(),
					Descriptor.GetCapabilityId(),
					MoveTemp(Namespace),
					MoveTemp(Method),
					Descriptor.GetDomainId(),
					Descriptor.GetIdentity());
				if (!Route.IsValid())
				{
					OutError =
						TEXT(
							"simple_authoring_route_invalid:")
						+ Descriptor.GetIdentity()
							.GetDescriptorId();
					return false;
				}
				const FString PythonBinding =
					Route.Namespace + TEXT(".") + Route.Method;
				if (CommandIds.Contains(Route.CommandId)
					|| DescriptorIds.Contains(
						Route.DescriptorIdentity
							.GetDescriptorId())
					|| PythonBindings.Contains(PythonBinding))
				{
					OutError =
						TEXT(
							"simple_authoring_route_duplicate:")
						+ Route.CommandId;
					return false;
				}
				CommandIds.Add(Route.CommandId);
				DescriptorIds.Add(
					Route.DescriptorIdentity.GetDescriptorId());
				PythonBindings.Add(PythonBinding);
				OutRoutes.Add(MoveTemp(Route));
			}
			return OutRoutes.Num() == 104;
		}
	}

	FUeAgentSimpleAuthoringCommandRoute::
		FUeAgentSimpleAuthoringCommandRoute(
			FString InCommandId,
			FString InCapabilityId,
			FString InNamespace,
			FString InMethod,
			FString InFamilyId,
			FUeAgentDescriptorIdentity InDescriptorIdentity)
		: CommandId(MoveTemp(InCommandId))
		, CapabilityId(MoveTemp(InCapabilityId))
		, Namespace(MoveTemp(InNamespace))
		, Method(MoveTemp(InMethod))
		, FamilyId(MoveTemp(InFamilyId))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
	{
	}

	bool FUeAgentSimpleAuthoringCommandRoute::IsValid() const
	{
		return !CommandId.IsEmpty()
			&& CommandId == CapabilityId
			&& IsIdentifier(Namespace)
			&& IsIdentifier(Method)
			&& !FamilyId.IsEmpty()
			&& DescriptorIdentity.IsValid();
	}

	bool TryCreateSimpleAuthoringDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		TArray<FUeAgentOperationDescriptor> Descriptors;
		if (!TryCollectDescriptors(Descriptors, OutError))
		{
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}

	const TArray<FUeAgentSimpleAuthoringCommandRoute>&
	GetSimpleAuthoringCommandRoutes()
	{
		static const TArray<FUeAgentSimpleAuthoringCommandRoute>
			Routes = []()
			{
				TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
				FString Error;
				const bool bRegistryCreated =
					TryCreateSimpleAuthoringDescriptorRegistry(
						Registry,
						Error);
				TArray<FUeAgentSimpleAuthoringCommandRoute>
					Result;
				const bool bRoutesCreated =
					bRegistryCreated
					&& Registry.IsValid()
					&& TryBuildRoutes(*Registry, Result, Error);
				checkf(
					bRoutesCreated,
					TEXT(
						"Simple Authoring command routes invalid: %s"),
					*Error);
				return Result;
			}();
		return Routes;
	}

	bool TryCreateProductionCommandContractRegistry(
		TSharedPtr<const FUeAgentCommandContractRegistry>&
			OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry>
			DescriptorRegistry;
		if (!TryCreateSimpleAuthoringDescriptorRegistry(
				DescriptorRegistry,
				OutError)
			|| !DescriptorRegistry.IsValid())
		{
			return false;
		}

		TArray<FUeAgentCommandContract> Contracts =
			GetDefaultCommandContractRegistry()->GetContracts();
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes =
			GetSimpleAuthoringCommandRoutes();
		for (const FUeAgentSimpleAuthoringCommandRoute& Route :
			Routes)
		{
			const FUeAgentOperationDescriptor* Descriptor =
				DescriptorRegistry->FindExact(
					Route.DescriptorIdentity);
			if (!Descriptor)
			{
				OutError =
					TEXT(
						"simple_authoring_route_descriptor_missing:")
					+ Route.CommandId;
				return false;
			}
			TArray<FUeAgentCommandParameter> Parameters;
			if (!TryProjectParameters(
					*Descriptor,
					Parameters,
					OutError))
			{
				return false;
			}
			Contracts.Emplace(
				Route.CommandId,
				Route.Namespace,
				Route.Method,
				MoveTemp(Parameters),
				1,
				1,
				EUeAgentCommandStatus::Active);
		}
		return FUeAgentCommandContractRegistry::TryCreate(
			MoveTemp(Contracts),
			OutRegistry,
			OutError);
	}
}
