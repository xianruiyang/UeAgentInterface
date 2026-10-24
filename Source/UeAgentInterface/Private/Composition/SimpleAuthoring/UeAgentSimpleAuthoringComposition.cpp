#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"
#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	using namespace CommandContract;
	using namespace Contract;

	namespace
	{
		using FDescriptorRegistryFactory = bool (*)(
			TSharedPtr<const FUeAgentDescriptorRegistry>&,
			UE::UAI::Contract::FDiagnostic&);

		struct FFamilySource
		{
			const TCHAR* FamilyId;
			FDescriptorRegistryFactory Factory;
		};

		const FFamilySource FamilySources[] = {
			{
				TEXT("world"),
				&DomainAdapters::WorldSimpleEffect::
					TryCreateWorldSimpleEffectDescriptorRegistry,
			},
			{
				TEXT("landscape"),
				&DomainAdapters::LandscapeGeometryEffect::
					TryCreateLandscapeGeometryEffectDescriptorRegistry,
			},
			{
				TEXT("mesh"),
				&DomainAdapters::ModelingGeometryEffect::
					TryCreateModelingGeometryEffectDescriptorRegistry,
			},
			{
				TEXT("asset_data"),
				&DomainAdapters::AssetDataSimpleEffect::
					TryCreateAssetDataSimpleEffectDescriptorRegistry,
			},
			{
				TEXT("editor_project"),
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
			FString& OutMethod,
			FDiagnostic& OutError)
		{
			OutNamespace.Reset();
			OutMethod.Reset();
			OutError.Reset();
			const auto Fail = [&](const FString& Field, const FString& Actual, const FString& Reason)
			{
				OutError = FDiagnostic::Failure(TEXT("simple_authoring_route_invalid"), Reason, TEXT("registry"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = CapabilityId;
				OutError.PropertyPath = Field;
				OutError.ActualValue = MakeShared<FJsonValueString>(Actual);
				return false;
			};
			TArray<FString> Segments;
			CapabilityId.ParseIntoArray(Segments, TEXT("."), true);
			if (Segments.Num() < 2)
				return Fail(TEXT("capability_id"), CapabilityId,
					TEXT("The capability ID must contain at least two non-empty dot-separated segments."));
			OutNamespace = Segments[0];
			Segments.RemoveAt(0);
			OutMethod = FString::Join(Segments, TEXT("_"));
			if (!IsIdentifier(OutNamespace))
				return Fail(TEXT("namespace"), OutNamespace,
					TEXT("The generated Python namespace must start with a letter or '_' and contain only letters, digits or '_'."));
			if (!IsIdentifier(OutMethod))
				return Fail(TEXT("method"), OutMethod,
					TEXT("The generated Python method must start with a letter or '_' and contain only letters, digits or '_'."));
			return true;
		}

		bool TryCollectDescriptors(
			TArray<FUeAgentOperationDescriptor>& OutDescriptors,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutDescriptors.Reset();
			OutError.Reset();
			for (const FFamilySource& Source : FamilySources)
			{
				TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
				UE::UAI::Contract::FDiagnostic FamilyError;
				if (!Source.Factory(Registry, FamilyError))
				{
					OutError = MoveTemp(FamilyError);
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Source.FamilyId;
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
						const bool bDomainMismatch = Descriptor.GetDomainId() != Source.FamilyId;
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_family_descriptor_mismatch"),
							bDomainMismatch ? TEXT("The descriptor belongs to a different authoring family.")
								: TEXT("The authoring family requires an operation descriptor."), TEXT("registration"),
							Descriptor.GetIdentity().GetDescriptorId());
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Source.FamilyId;
						OutError.PropertyPath = bDomainMismatch ? TEXT("/domain_id") : TEXT("/operation_kind");
						OutError.ActualValue = MakeShared<FJsonValueString>(bDomainMismatch
							? Descriptor.GetDomainId()
							: Descriptor.GetOperationKind() == EUeAgentDescriptorOperationKind::Query ? TEXT("query") : TEXT("invalid"));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(bDomainMismatch ? Source.FamilyId : TEXT("operation"));
						return false;
					}
					OutDescriptors.Add(Descriptor);
				}
			}
			return true;
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

		struct FRootVariant
		{
			TSharedPtr<FJsonObject> Schema;
			FString Pointer;
		};

		void AddRootVariants(
			const TSharedPtr<FJsonObject>& Schema,
			TArray<FRootVariant>& OutVariants)
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
				OutVariants.Add({Schema, FString()});
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* OneOf = nullptr;
			if (!Schema->TryGetArrayField(TEXT("oneOf"), OneOf)
				|| !OneOf)
			{
				return;
			}
			for (int32 Index = 0; Index < OneOf->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*OneOf)[Index];
				const TSharedPtr<FJsonObject>* Variant = nullptr;
				if (Value.IsValid()
					&& Value->TryGetObject(Variant)
					&& Variant
					&& Variant->IsValid())
				{
					OutVariants.Add({*Variant, TEXT("/oneOf/") + FString::FromInt(Index)});
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutParameters.Reset();
			TSharedPtr<FJsonObject> Root;
			if (!UE::UAI::Contract::Json::TryParseObject(Descriptor.GetInputSchema().GetJsonValue(), Root, OutError))
			{
				OutError.SourceKind = TEXT("native_result");
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("simple_authoring_input_schema_parse_failed"),
					OutError, TEXT("registry"), TEXT("The registered input schema cannot be read as a JSON object."));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("/input_schema");
				OutError.TargetPath = Descriptor.GetDomainId();
				OutError.InternalContext = Descriptor.GetIdentity().GetDescriptorId();
				return false;
			}

			TArray<FRootVariant> Variants;
			AddRootVariants(Root, Variants);
			if (Variants.IsEmpty())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_input_schema_root_unsupported"),
					TEXT("The input schema has neither a properties object nor an object variant in oneOf."), TEXT("registry"),
					Descriptor.GetIdentity().GetDescriptorId());
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Descriptor.GetDomainId();
				OutError.PropertyPath = TEXT("/input_schema");
				const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
				Shape->SetStringField(TEXT("properties_type"), UeAgentJsonDiagnostics::JsonValueTypeToString(Root->Values.FindRef(TEXT("properties"))));
				Shape->SetStringField(TEXT("oneOf_type"), UeAgentJsonDiagnostics::JsonValueTypeToString(Root->Values.FindRef(TEXT("oneOf"))));
				Shape->SetNumberField(TEXT("object_variant_count"), Variants.Num());
				OutError.ActualValue = MakeShared<FJsonValueObject>(Shape);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("properties object or at least one object variant"));
				return false;
			}

			TMap<FString, TArray<TSharedPtr<FJsonObject>>>
				SchemasByName;
			TMap<FString, int32> RequiredCountByName;
			for (const FRootVariant& Entry : Variants)
			{
				const TSharedPtr<FJsonObject>& Variant = Entry.Schema;
				const TSharedPtr<FJsonObject>* Properties = nullptr;
				if (!Variant->TryGetObjectField(
						TEXT("properties"),
						Properties)
					|| !Properties
					|| !Properties->IsValid())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_input_variant_invalid"),
						TEXT("The input schema variant requires a properties object."), TEXT("registry"),
						Descriptor.GetIdentity().GetDescriptorId());
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Descriptor.GetDomainId();
					OutError.PropertyPath = TEXT("/input_schema") + Entry.Pointer + TEXT("/properties");
					OutError.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Variant->Values.FindRef(TEXT("properties"))));
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
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
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_property_schema_invalid"),
							TEXT("A registered parameter's schema must be an object."), TEXT("registry"),
							Descriptor.GetIdentity().GetDescriptorId());
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Descriptor.GetDomainId();
						OutError.PropertyPath = TEXT("/input_schema") + Entry.Pointer + TEXT("/properties/")
							+ Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						OutError.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Pair.Value));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
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
				TArray<TSharedPtr<FJsonValue>> Alternatives;
				TSet<FString> SeenSchemas;
				for (const TSharedPtr<FJsonObject>& Schema : SchemasByName.FindChecked(Name))
				{
					FString Canonical;
					if (!Json::TryCanonicalize(Schema, Canonical, OutError)) return false;
					if (!SeenSchemas.Contains(Canonical))
					{
						SeenSchemas.Add(Canonical);
						Alternatives.Add(MakeShared<FJsonValueObject>(Schema));
					}
				}
				TSharedPtr<FJsonObject> ParameterSchema;
				if (Alternatives.Num() == 1)
				{
					ParameterSchema = Alternatives[0]->AsObject();
				}
				else
				{
					ParameterSchema = MakeShared<FJsonObject>();
					ParameterSchema->SetArrayField(TEXT("anyOf"), MoveTemp(Alternatives));
				}
				FString SchemaJson;
				if (!Json::TryCanonicalize(ParameterSchema, SchemaJson, OutError)) return false;
				OutParameters.Emplace(
					Name,
					WireType,
					bRequired,
					bNullable,
					MoveTemp(SchemaJson));
			}
			return true;
		}

		bool TryBuildRoutes(
			const FUeAgentDescriptorRegistry& Registry,
			TArray<FUeAgentSimpleAuthoringCommandRoute>& OutRoutes,
			UE::UAI::Contract::FDiagnostic& OutError)
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
						Method,
						OutError))
				{
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Descriptor.GetIdentity().GetDescriptorId();
					return false;
				}
				FUeAgentSimpleAuthoringCommandRoute Route(
					Descriptor.GetCapabilityId(),
					Descriptor.GetCapabilityId(),
					MoveTemp(Namespace),
					MoveTemp(Method),
					Descriptor.GetDomainId(),
					Descriptor.GetIdentity());
				if (!Route.TryValidate(OutError))
				{
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
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
					const bool bCommandDuplicate = CommandIds.Contains(Route.CommandId);
					const bool bDescriptorDuplicate = !bCommandDuplicate
						&& DescriptorIds.Contains(Route.DescriptorIdentity.GetDescriptorId());
					OutError = FDiagnostic::Failure(TEXT("simple_authoring_route_duplicate"),
						bCommandDuplicate ? TEXT("More than one authoring route uses this command ID.")
							: bDescriptorDuplicate ? TEXT("More than one authoring route uses this descriptor ID.")
								: TEXT("More than one authoring route uses this Python namespace and method."), TEXT("registry"));
					OutError.SourceKind = TEXT("native_result");
					OutError.CandidateName = Route.CommandId;
					OutError.PropertyPath = bCommandDuplicate ? TEXT("command_id")
						: bDescriptorDuplicate ? TEXT("descriptor_id") : TEXT("python_binding");
					OutError.ActualValue = MakeShared<FJsonValueString>(bCommandDuplicate ? Route.CommandId
						: bDescriptorDuplicate ? Route.DescriptorIdentity.GetDescriptorId() : PythonBinding);
					return false;
				}
				CommandIds.Add(Route.CommandId);
				DescriptorIds.Add(
					Route.DescriptorIdentity.GetDescriptorId());
				PythonBindings.Add(PythonBinding);
				OutRoutes.Add(MoveTemp(Route));
			}
			return true;
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
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentSimpleAuthoringCommandRoute::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (CommandId.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_route_invalid"),
				TEXT("The route requires a non-empty command_id."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("command_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
			return false;
		}
		if (CommandId != CapabilityId)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_route_invalid"),
				TEXT("The route capability must match its command ID."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("capability_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(CommandId);
			OutError.ActualValue = MakeShared<FJsonValueString>(CapabilityId);
			return false;
		}
		if (!IsIdentifier(Namespace))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_route_invalid"),
				TEXT("The Python namespace is not a valid identifier."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("namespace");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("identifier"));
			OutError.ActualValue = MakeShared<FJsonValueString>(Namespace);
			return false;
		}
		if (!IsIdentifier(Method))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_route_invalid"),
				TEXT("The Python method is not a valid identifier."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("method");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("identifier"));
			OutError.ActualValue = MakeShared<FJsonValueString>(Method);
			return false;
		}
		if (FamilyId.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_route_invalid"),
				TEXT("The route requires a non-empty family_id."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("family_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
			OutError.ActualValue = MakeShared<FJsonValueString>(FamilyId);
			return false;
		}
		if (!DescriptorIdentity.IsValid())
		{
			OutError = DescriptorIdentity.GetValidationDiagnostic();
			OutError.SubjectId = CommandId;
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			return false;
		}
		return true;
	}

	bool TryCreateSimpleAuthoringDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
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
				UE::UAI::Contract::FDiagnostic Error;
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
					*Error.ToPublicDetail());
				return Result;
			}();
		return Routes;
	}

	bool TryCreateProductionCommandContractRegistry(
		TSharedPtr<const FUeAgentCommandContractRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry>
			DescriptorRegistry;
		if (!TryCreateSimpleAuthoringDescriptorRegistry(
				DescriptorRegistry,
				OutError))
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
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_route_descriptor_missing"),
					TEXT("The command route references a descriptor that is not registered."), TEXT("registry"),
					Route.DescriptorIdentity.GetDescriptorId());
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Route.CommandId;
				OutError.PropertyPath = TEXT("/descriptor_exists");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
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
