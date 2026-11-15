#include "Domains/Material/Authoring/UeAgentMaterialInstanceAuthoring.h"

#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Engine/TextureCollection.h"
#include "Engine/Font.h"
#include "VT/RuntimeVirtualTexture.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/ObjectVersion.h"
#include "Materials/MaterialFunctionInstance.h"
#include "MaterialEditingLibrary.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Materials/MaterialLayersFunctions.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "UObject/UnrealType.h"
#include <type_traits>

namespace UeAgentMaterialAuthoring
{
	using namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph;
	namespace
	{
		UE::UAI::Contract::FDiagnostic ParameterFailure(const TCHAR* Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected, const FJsonObject* Source = nullptr)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("material_instance_authoring"));
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValue = Expected;
			if (Source) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Source, Field);
			return Error;
		}

		UE::UAI::Contract::FDiagnostic UnsupportedParameterKind(const FString& Kind, const FString& Name)
		{
			return ParameterFailure(TEXT("material_instance_parameter_kind_unsupported"),
				FString::Printf(TEXT("Material parameter '%s' has unsupported kind '%s'."), *Name, *Kind),
				TEXT("parameter_kind"), MakeShared<FJsonValueString>(Kind),
				MakeShared<FJsonValueString>(TEXT("scalar, vector, texture, static_switch, double_vector, texture_collection, runtime_virtual_texture, sparse_volume_texture, font, static_component_mask")));
		}
		UeAgentPropertyValueCodec::FOptions CompoundValueOptions()
		{
			UeAgentPropertyValueCodec::FOptions Options;
			Options.bAllowPartialStructFields = true;
			Options.bCaptureStructFieldDescriptors = true;
			Options.StructFieldFilter = [](const UScriptStruct* Struct, const FProperty* Field) {
				const FName Name = Field->GetFName();
				return Struct == FFontParameterValue::StaticStruct()
					? Name == TEXT("FontValue") || Name == TEXT("FontPage")
					: Struct == FStaticComponentMaskParameter::StaticStruct()
						&& (Name == TEXT("R") || Name == TEXT("G") || Name == TEXT("B") || Name == TEXT("A"));
			};
			return Options;
		}

		template <typename T>
		bool CaptureCompound(UObject& Owner, const T& Carrier,
			TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonValue>& Definition, UE::UAI::Contract::FDiagnostic& Error)
		{
			const auto Filter = CompoundValueOptions().StructFieldFilter;
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			for (TFieldIterator<FProperty> It(T::StaticStruct(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				if (!Filter(T::StaticStruct(), *It)) continue;
				UeAgentAuthoringTargetLocator::FResolveResult Resolve;
				auto& Request = Requests.AddDefaulted_GetRef();
				if (!UeAgentAuthoringState::BuildStructPropertyReadRequest(T::StaticStruct(), &Carrier,
					Owner.GetPathName(), &Owner, It->GetName(), TEXT("core.property"), Request, Resolve))
				{ Error = Resolve.Diagnostic; return false; }
				Request.Limits.AuthorizeProperty = [Filter](const FProperty* Field) { return Filter(T::StaticStruct(), Field); };
			}
			UeAgentAuthoringState::FCaptureResult Capture;
			TSharedPtr<FJsonObject> Properties, Definitions;
			UE::UAI::HjsonWorkspace::FWorkspaceError ProjectionError;
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
			{ Error = Capture.Diagnostic; return false; }
			if (!UE::UAI::HjsonWorkspace::TryProjectReflectedRecord(Capture.Snapshot, Properties, Definitions, ProjectionError))
			{ Error = ProjectionError.GetDiagnostic(); return false; }
			auto Fields = MakeShared<FJsonObject>();
			auto FieldDefinitions = MakeShared<FJsonObject>();
			for (const auto& Pair : Properties->Values)
			{
				const auto Row = Pair.Value->AsObject();
				Fields->SetField(Pair.Key, Row->TryGetField(TEXT("value")));
				FieldDefinitions->SetField(Pair.Key, Definitions->TryGetField(Row->GetStringField(TEXT("definition"))));
			}
			auto Record = MakeShared<FJsonObject>(); Record->SetObjectField(TEXT("fields"), Fields);
			auto Schema = MakeShared<FJsonObject>(); Schema->SetObjectField(TEXT("fields"), FieldDefinitions);
			Value = MakeShared<FJsonValueObject>(Record);
			Definition = MakeShared<FJsonValueObject>(Schema);
			return true;
		}

		template <typename T>
		void MergeLocalParameters(const TArray<T>& Local, TMap<FMaterialParameterInfo, FMaterialParameterMetadata>& Parameters)
		{
			for (const auto& Parameter : Local)
			{
				if (!Parameter.IsOverride()) continue;
				auto& Meta = Parameters.FindOrAdd(Parameter.ParameterInfo);
				Parameter.GetValue(Meta);
				Meta.bOverride = true;
			}
		}
		// The native carrier identifies the real value property. Capture, ordinary
		// projection, reference resolution and decoding stay in the shared owners.
		template <typename T>
		bool CaptureValue(UObject& Owner, T& Carrier, const FName Field,
			TSharedPtr<FJsonValue>& OutValue, TSharedPtr<FJsonValue>& OutDefinition, UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringState::FReadRequest Request;
			UeAgentAuthoringTargetLocator::FResolveResult Resolve;
			if (!UeAgentAuthoringState::BuildStructPropertyReadRequest(T::StaticStruct(), &Carrier,
				Owner.GetPathName(), &Owner, Field.ToString(), TEXT("core.property"), Request, Resolve))
			{ OutError = Resolve.Diagnostic; return false; }
			Request.Limits.AuthorizeProperty = [Field](const FProperty* Property) { return Property->GetFName() == Field; };
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!UeAgentAuthoringState::CaptureSnapshot({Request}, Capture))
			{ OutError = Capture.Diagnostic; return false; }
			TSharedPtr<FJsonObject> Properties, Definitions;
			UE::UAI::HjsonWorkspace::FWorkspaceError Error;
			if (!UE::UAI::HjsonWorkspace::TryProjectReflectedRecord(Capture.Snapshot, Properties, Definitions, Error, false))
			{ OutError = Error.GetDiagnostic(); return false; }
			const auto Row = Properties->GetObjectField(Field.ToString());
			OutValue = Row->TryGetField(TEXT("value"));
			OutDefinition = Definitions->TryGetField(Row->GetStringField(TEXT("definition")));
			if (!OutValue.IsValid() || !OutDefinition.IsValid()) {
				OutError = ParameterFailure(TEXT("material_instance_value_projection_incomplete"),
					FString::Printf(TEXT("Material parameter property '%s' has no projected %s."), *Field.ToString(),
						!OutValue ? TEXT("value") : TEXT("definition")), Field.ToString(),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Owner.GetPathName();
				return false;
			}
			return true;
		}

		bool CaptureParameterValue(UMaterialInstanceConstant& Instance, const FMaterialParameterInfo& Info,
			const FMaterialParameterValue& Native, FString& Kind, TSharedPtr<FJsonValue>& Value,
			TSharedPtr<FJsonValue>& Definition, UE::UAI::Contract::FDiagnostic& Error)
		{
			switch (Native.Type)
			{
			case EMaterialParameterType::Scalar:
			{
				Kind = TEXT("scalar"); FScalarParameterValue Carrier;
				Carrier.ParameterValue = Native.AsScalar();
				return CaptureValue(Instance, Carrier, GET_MEMBER_NAME_CHECKED(FScalarParameterValue, ParameterValue), Value, Definition, Error);
			}
			case EMaterialParameterType::Vector:
			{
				Kind = TEXT("vector"); FVectorParameterValue Carrier;
				Carrier.ParameterValue = Native.AsLinearColor();
				return CaptureValue(Instance, Carrier, GET_MEMBER_NAME_CHECKED(FVectorParameterValue, ParameterValue), Value, Definition, Error);
			}
			case EMaterialParameterType::Texture:
			{
				Kind = TEXT("texture"); FTextureParameterValue Carrier;
				Carrier.ParameterValue = Native.Texture;
				return CaptureValue(Instance, Carrier, GET_MEMBER_NAME_CHECKED(FTextureParameterValue, ParameterValue), Value, Definition, Error);
			}
			case EMaterialParameterType::StaticSwitch:
			{
				Kind = TEXT("static_switch"); FStaticSwitchParameter Carrier;
				Carrier.Value = Native.AsStaticSwitch();
				return CaptureValue(Instance, Carrier, GET_MEMBER_NAME_CHECKED(FStaticSwitchParameter, Value), Value, Definition, Error);
			}
			case EMaterialParameterType::DoubleVector:
			{
				Kind = TEXT("double_vector"); FDoubleVectorParameterValue Carrier;
				Carrier.ParameterValue = Native.AsVector4d();
				return CaptureValue(Instance, Carrier, GET_MEMBER_NAME_CHECKED(FDoubleVectorParameterValue, ParameterValue), Value, Definition, Error);
			}
			case EMaterialParameterType::TextureCollection:
			{
				Kind = TEXT("texture_collection"); FTextureCollectionParameterValue Carrier;
				Carrier.ParameterValue = Native.TextureCollection;
				return CaptureValue(Instance, Carrier, GET_MEMBER_NAME_CHECKED(FTextureCollectionParameterValue, ParameterValue), Value, Definition, Error);
			}
			case EMaterialParameterType::RuntimeVirtualTexture:
			{
				Kind = TEXT("runtime_virtual_texture"); FRuntimeVirtualTextureParameterValue Carrier;
				Carrier.ParameterValue = Native.RuntimeVirtualTexture;
				return CaptureValue(Instance, Carrier, GET_MEMBER_NAME_CHECKED(FRuntimeVirtualTextureParameterValue, ParameterValue), Value, Definition, Error);
			}
			case EMaterialParameterType::SparseVolumeTexture:
			{
				Kind = TEXT("sparse_volume_texture"); FSparseVolumeTextureParameterValue Carrier;
				Carrier.ParameterValue = Native.SparseVolumeTexture;
				return CaptureValue(Instance, Carrier, GET_MEMBER_NAME_CHECKED(FSparseVolumeTextureParameterValue, ParameterValue), Value, Definition, Error);
			}
			case EMaterialParameterType::Font:
			{
				Kind = TEXT("font"); FFontParameterValue Carrier(Info, Native.Font.Value, Native.Font.Page);
				return CaptureCompound(Instance, Carrier, Value, Definition, Error);
			}
			case EMaterialParameterType::StaticComponentMask:
			{
				Kind = TEXT("static_component_mask"); FStaticComponentMaskParameter Carrier;
				const auto Mask = Native.AsStaticComponentMask();
				Carrier.R = Mask.R; Carrier.G = Mask.G; Carrier.B = Mask.B; Carrier.A = Mask.A;
				return CaptureCompound(Instance, Carrier, Value, Definition, Error);
			}
			default:
				Error = UnsupportedParameterKind(FString::FromInt(static_cast<int32>(Native.Type)), Info.Name.ToString());
				Error.SourceKind = TEXT("native_result");
				return false;
			}
		}

		bool AppendParameterDefault(UMaterialInstanceConstant& Instance, EMaterialParameterType Type,
			const FMaterialParameterInfo& Info, TArray<FInstanceParameterDefault>* Defaults,
			UE::UAI::Contract::FDiagnostic& Error)
		{
			if (!Defaults) return true;
			auto& Default = Defaults->AddDefaulted_GetRef();
			if (Type == EMaterialParameterType::Scalar && Info.Association == EMaterialParameterAssociation::GlobalParameter
				&& Info.Index == INDEX_NONE)
			{
				static const FName DepthBias(TEXT("RefractionDepthBias")), LegacyBias(TEXT("RefractionBias"));
				const FName NativeName = Instance.GetLinkerUEVersion() >= VER_UE4_REFRACTION_BIAS_TO_REFRACTION_DEPTH_BIAS ? DepthBias : LegacyBias;
				if (Info.Name == NativeName && Instance.ScalarParameterValues.ContainsByPredicate([&](const FScalarParameterValue& Local) {
					return Local.ParameterInfo == Info;
				}))
				{
					// The material instance editor resets this local scalar through
					// the parent's native refraction setting, outside the expression parameter tree.
					float Bias = 0.f;
					if (!Instance.Parent || !Instance.Parent->GetRefractionSettings(Bias)) return true;
					Default.SourceIdentity = Instance.Parent->GetPathName();
					Default.Provenance = TEXT("UMaterialInstance parent GetRefractionSettings; excludes local refraction bias override");
					FString Kind;
					TSharedPtr<FJsonValue> Definition;
					return CaptureParameterValue(Instance, Info, FMaterialParameterValue(Bias), Kind, Default.Value, Definition, Error);
				}
			}
			Default.SourceIdentity = Instance.GetPathName();
			Default.Provenance = TEXT("UMaterialInterface.GetParameterDefaultValue; inherited overrides and native layer mapping; excludes local override");
			FMaterialParameterMetadata NativeDefault;
			// The engine skips this instance's override, retains parent overrides,
			// and remaps layer indices while traversing the native inheritance chain.
			if (!Instance.GetParameterDefaultValue(Type, Info, NativeDefault)) return true;
			FString Kind;
			TSharedPtr<FJsonValue> Definition;
			return CaptureParameterValue(Instance, Info, NativeDefault.Value, Kind, Default.Value, Definition, Error);
		}

		template <typename T>
		bool DecodeValue(T& Carrier, const FName Field, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
		{
			const auto Descriptor = MakeShared<FJsonObject>();
			Descriptor->SetStringField(TEXT("property_path"), Field.ToString());
			Descriptor->SetField(TEXT("value"), Value);
			UeAgentAuthoringObjectApply::FOptions Options;
			Options.bCompactValues = true;
			Options.bEmitEditorNotifications = false;
			Options.bMarkPackageDirty = false;
			Options.AuthorizeProperty = [Field](const FProperty* Property) { return Property->GetFName() == Field; };
			UeAgentAuthoringObjectApply::FPreparedApply Prepared;
			UeAgentAuthoringObjectApply::FFailure Failure;
			if (!UeAgentAuthoringObjectApply::PrepareStruct(T::StaticStruct(), &Carrier, nullptr,
				{MakeShared<FJsonValueObject>(Descriptor)}, Options, Prepared, Failure))
			{ OutError = Failure.Diagnostic; return false; }
			UeAgentAuthoringObjectApply::FApplyResult Result;
			if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Result))
			{ OutError = Result.Diagnostic; return false; }
			return true;
		}
	}

	bool CaptureInstanceParameters(UMaterialInstanceConstant& Instance,
		TArray<TSharedPtr<FJsonValue>>& OutParameters, UE::UAI::Contract::FDiagnostic& OutError,
		TArray<TSharedPtr<FJsonValue>>* OutDefinitions,
		TArray<FInstanceParameterDefault>* OutDefaults)
	{
		OutParameters.Reset();
		if (OutDefinitions) OutDefinitions->Reset();
		if (OutDefaults) OutDefaults->Reset();
		TArray<FMaterialNiagaraGraphEffectiveParameter> Parameters;
		FString Revision, Evidence;
		FMaterialNiagaraGraphAdapterError Error;
		if (!FMaterialNiagaraGraphNativeHook().TryCaptureMaterialInstanceEffectiveParameters(
			Instance, Parameters, Revision, Evidence, Error, true))
		{ OutError = Error; return false; }
		// Keep local overrides even when the current parent no longer declares them.
		// They remain native asset state and must not vanish from ordinary authoring.
		auto AddLocal = [&](const FMaterialParameterInfo& Info, EMaterialNiagaraGraphParameterType Kind,
			float Scalar, FLinearColor Vector, FString Texture, bool bSwitch)
		{
			FMaterialNiagaraGraphParameterIdentity Identity(Info.Name.ToString(),
				Info.Association == EMaterialParameterAssociation::GlobalParameter ? TEXT("global")
				: Info.Association == EMaterialParameterAssociation::LayerParameter ? TEXT("layer") : TEXT("blend"), Info.Index, Kind);
			if (!Parameters.ContainsByPredicate([&](const auto& P) { return P.GetIdentity().GetParameterKey() == Identity.GetParameterKey(); }))
				Parameters.Emplace(MoveTemp(Identity), true, Scalar, Vector, MoveTemp(Texture), bSwitch);
		};
		for (const auto& P : Instance.ScalarParameterValues) AddLocal(P.ParameterInfo, EMaterialNiagaraGraphParameterType::Scalar, P.ParameterValue, FLinearColor::Black, {}, false);
		for (const auto& P : Instance.VectorParameterValues) AddLocal(P.ParameterInfo, EMaterialNiagaraGraphParameterType::Vector, 0, P.ParameterValue, {}, false);
		for (const auto& P : Instance.TextureParameterValues) AddLocal(P.ParameterInfo, EMaterialNiagaraGraphParameterType::Texture, 0, FLinearColor::Black, P.ParameterValue ? P.ParameterValue->GetPathName() : FString(), false);
		const FStaticParameterSet LocalStaticParameters = Instance.GetStaticParameters();
		for (const auto& P : LocalStaticParameters.StaticSwitchParameters)
			if (P.bOverride) AddLocal(P.ParameterInfo, EMaterialNiagaraGraphParameterType::StaticSwitch, 0, FLinearColor::Black, {}, P.Value);
		for (int32 ParameterIndex = 0; ParameterIndex < Parameters.Num(); ++ParameterIndex)
		{
			const auto& Parameter = Parameters[ParameterIndex];
			if (!Parameter.IsValid())
			{
				OutError = *Parameter.GetValidationDiagnostic();
				OutError.PropertyPath = TEXT("/parameters/") + FString::FromInt(ParameterIndex) + OutError.PropertyPath;
				return false;
			}
			const auto& Identity = Parameter.GetIdentity();
			auto Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("parameter_name"), Identity.GetParameterName());
			Value->SetStringField(TEXT("association"), Identity.GetParameterAssociation());
			Value->SetNumberField(TEXT("parameter_index"), Identity.GetParameterIndex());
			Value->SetBoolField(TEXT("authored_override"), Parameter.IsOverridden());
			TSharedPtr<FJsonValue> NativeValue, Definition;
			const FMaterialParameterInfo Info(FName(*Identity.GetParameterName()),
				Identity.GetParameterAssociation() == TEXT("global") ? EMaterialParameterAssociation::GlobalParameter
				: Identity.GetParameterAssociation() == TEXT("layer") ? EMaterialParameterAssociation::LayerParameter : EMaterialParameterAssociation::BlendParameter,
				Identity.GetParameterIndex());
			FMaterialParameterValue Native;
			switch (Identity.GetParameterType())
			{
			case EMaterialNiagaraGraphParameterType::Scalar:
				Native = FMaterialParameterValue(Parameter.GetScalarValue());
				break;
			case EMaterialNiagaraGraphParameterType::Vector:
				Native = FMaterialParameterValue(Parameter.GetVectorValue());
				break;
			case EMaterialNiagaraGraphParameterType::Texture:
			{
				UTexture* Texture = nullptr;
				Instance.GetTextureParameterValue(Info, Texture);
				if (const auto* Local = Instance.TextureParameterValues.FindByPredicate([&](const auto& P) { return P.ParameterInfo == Info; })) Texture = Local->ParameterValue;
				Native = FMaterialParameterValue(Texture);
				break;
			}
			case EMaterialNiagaraGraphParameterType::StaticSwitch:
				Native = FMaterialParameterValue(Parameter.GetBooleanValue());
				break;
			default:
				OutError = UnsupportedParameterKind(FString::FromInt(static_cast<int32>(Identity.GetParameterType())), Identity.GetParameterName());
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			FString Kind;
			if (!CaptureParameterValue(Instance, Info, Native, Kind, NativeValue, Definition, OutError)) return false;
			Value->SetStringField(TEXT("parameter_kind"), Kind);
			Value->SetField(TEXT("value"), NativeValue);
			if (OutDefinitions) OutDefinitions->Add(Definition);
			OutParameters.Add(MakeShared<FJsonValueObject>(Value));
			if (!AppendParameterDefault(Instance, Native.Type, Info, OutDefaults, OutError)) return false;
		}
		for (const EMaterialParameterType Type : {EMaterialParameterType::DoubleVector, EMaterialParameterType::TextureCollection,
			EMaterialParameterType::Font, EMaterialParameterType::RuntimeVirtualTexture,
			EMaterialParameterType::SparseVolumeTexture, EMaterialParameterType::StaticComponentMask})
		{
			TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Values;
			Instance.GetAllParametersOfType(Type, Values);
			switch (Type)
			{
			case EMaterialParameterType::DoubleVector: MergeLocalParameters(Instance.DoubleVectorParameterValues, Values); break;
			case EMaterialParameterType::TextureCollection: MergeLocalParameters(Instance.TextureCollectionParameterValues, Values); break;
			case EMaterialParameterType::Font: MergeLocalParameters(Instance.FontParameterValues, Values); break;
			case EMaterialParameterType::RuntimeVirtualTexture: MergeLocalParameters(Instance.RuntimeVirtualTextureParameterValues, Values); break;
			case EMaterialParameterType::SparseVolumeTexture: MergeLocalParameters(Instance.SparseVolumeTextureParameterValues, Values); break;
			case EMaterialParameterType::StaticComponentMask: MergeLocalParameters(LocalStaticParameters.EditorOnly.StaticComponentMaskParameters, Values); break;
			default: break;
			}
			for (const auto& Pair : Values)
			{
				const auto& Info = Pair.Key;
				const auto& Meta = Pair.Value;
				FString Kind;
				TSharedPtr<FJsonValue> NativeValue, Definition;
				if (!CaptureParameterValue(Instance, Info, Meta.Value, Kind, NativeValue, Definition, OutError)) return false;
				auto Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("parameter_kind"), Kind);
				Value->SetStringField(TEXT("parameter_name"), Info.Name.ToString());
				Value->SetStringField(TEXT("association"), Info.Association == EMaterialParameterAssociation::GlobalParameter ? TEXT("global")
					: Info.Association == EMaterialParameterAssociation::LayerParameter ? TEXT("layer") : TEXT("blend"));
				Value->SetNumberField(TEXT("parameter_index"), Info.Index);
				Value->SetBoolField(TEXT("authored_override"), Meta.bOverride);
				Value->SetField(TEXT("value"), NativeValue);
				OutParameters.Add(MakeShared<FJsonValueObject>(Value));
				if (OutDefinitions) OutDefinitions->Add(Definition);
				if (!AppendParameterDefault(Instance, Type, Info, OutDefaults, OutError)) return false;
			}
		}
		return true;
	}

	bool PrepareInstanceParameters(UMaterialInstanceConstant& Instance,
		const TArray<TSharedPtr<FJsonValue>>& Parameters,
		TArray<FInstanceParameterEdit>& OutEdits, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutEdits.Reset();
		TArray<TSharedPtr<FJsonValue>> Current;
		if (!CaptureInstanceParameters(Instance, Current, OutError)) return false;
		TSet<FString> Seen;
		for (const auto& Parameter : Parameters)
		{
			const auto Value = Parameter.IsValid() && Parameter->Type == EJson::Object ? Parameter->AsObject() : nullptr;
			FInstanceParameterEdit Edit;
			Edit.Source = Value;
			if (Value) Edit.SourceLocations = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Value);
			ON_SCOPE_EXIT {
				if (!OutError.IsEmpty() && Value)
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Value);
			};
			FString Name, Association;
			int32 Index = 0;
			bool bOverride = false;
			if (Value && !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Value,
				{{TEXT("parameter_kind"), EJson::String}, {TEXT("parameter_name"), EJson::String},
				 {TEXT("association"), EJson::String}, {TEXT("authored_override"), EJson::Boolean}}, OutError)) return false;
			const TCHAR* InvalidField = !Value ? TEXT("")
				: !Value->TryGetStringField(TEXT("parameter_kind"), Edit.Kind) ? TEXT("parameter_kind")
				: !Value->TryGetStringField(TEXT("parameter_name"), Name) ? TEXT("parameter_name")
				: !Value->TryGetStringField(TEXT("association"), Association) ? TEXT("association")
				: !Value->HasField(TEXT("parameter_index")) ? TEXT("parameter_index")
				: !Value->TryGetBoolField(TEXT("authored_override"), bOverride) ? TEXT("authored_override") : nullptr;
			if (InvalidField) {
				OutError = ParameterFailure(TEXT("material_instance_parameter_state_invalid"),
					Value ? FString::Printf(TEXT("Material parameter field '%s' is missing or has the wrong type."), InvalidField)
					      : TEXT("Each material parameter entry must be an object."),
					InvalidField, Value ? Value->TryGetField(InvalidField) : Parameter,
					MakeShared<FJsonValueString>(!Value ? TEXT("object") : FString(InvalidField) == TEXT("authored_override") ? TEXT("boolean")
					    : FString(InvalidField) == TEXT("parameter_index") ? TEXT("integer") : TEXT("string")), Value.Get());
				return false;
			}
			if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Value, TEXT("parameter_index"), Index, OutError)) return false;
			Edit.bOverride = bOverride;
			const auto* Existing = Current.FindByPredicate([&](const auto& Candidate) {
				const auto Object = Candidate->AsObject();
				return Object->GetStringField(TEXT("parameter_kind")) == Edit.Kind
					&& Object->GetStringField(TEXT("parameter_name")) == Name
					&& Object->GetStringField(TEXT("association")) == Association
					&& Object->GetNumberField(TEXT("parameter_index")) == Index;
			});
			if (!Existing) {
				OutError = ParameterFailure(TEXT("material_instance_parameter_identity_unresolved"),
					FString::Printf(TEXT("No material parameter matches kind '%s', name '%s', association '%s', index %d."),
						*Edit.Kind, *Name, *Association, Index), TEXT("parameter_name"),
					MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("existing parameter identity")), Value.Get());
				TArray<TSharedPtr<FJsonValue>> Choices;
				for (const auto& Candidate : Current) {
					auto Choice = MakeShared<FJsonObject>();
					for (const TCHAR* Field : {TEXT("parameter_kind"), TEXT("parameter_name"), TEXT("association"), TEXT("parameter_index")})
						Choice->SetField(Field, Candidate->AsObject()->TryGetField(Field));
					Choices.Add(MakeShared<FJsonValueObject>(Choice));
				}
				OutError.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Choices));
				return false;
			}
			const FString Key = Edit.Kind + TEXT(":") + Name + TEXT(":") + Association + TEXT(":") + FString::FromInt(Index);
			if (Seen.Contains(Key)) {
				OutError = ParameterFailure(TEXT("material_instance_parameter_duplicate"),
					FString::Printf(TEXT("Material parameter '%s' (%s, %s, index %d) is assigned more than once."), *Name, *Edit.Kind, *Association, Index),
					TEXT("parameter_name"), MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("one assignment per parameter")), Value.Get());
				return false;
			}
			Seen.Add(Key);
			Edit.Info = FMaterialParameterInfo(FName(*Name), Association == TEXT("global")
				? EMaterialParameterAssociation::GlobalParameter : Association == TEXT("layer")
				? EMaterialParameterAssociation::LayerParameter : EMaterialParameterAssociation::BlendParameter, Index);
			if (!Edit.bOverride)
			{
				// The inherited value is resolved by UE on the proposed instance.
				// The exported previous value is not a new local override.
				OutEdits.Add(MoveTemp(Edit));
				continue;
			}
			const auto Input = Value->TryGetField(TEXT("value"));
			ON_SCOPE_EXIT {
				if (!OutError.IsEmpty()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Value, TEXT("value"));
			};
			if (Edit.Kind == TEXT("scalar"))
			{
				FScalarParameterValue Carrier;
				if (!DecodeValue(Carrier, GET_MEMBER_NAME_CHECKED(FScalarParameterValue, ParameterValue), Input, OutError)) return false;
				Edit.Scalar = Carrier.ParameterValue;
			}
			else if (Edit.Kind == TEXT("vector"))
			{
				FVectorParameterValue Carrier;
				if (!DecodeValue(Carrier, GET_MEMBER_NAME_CHECKED(FVectorParameterValue, ParameterValue), Input, OutError)) return false;
				Edit.Vector = Carrier.ParameterValue;
			}
			else if (Edit.Kind == TEXT("texture"))
			{
				FTextureParameterValue Carrier;
				if (!DecodeValue(Carrier, GET_MEMBER_NAME_CHECKED(FTextureParameterValue, ParameterValue), Input, OutError)) return false;
				Edit.Texture.Reset(Carrier.ParameterValue.Get());
			}
			else if (Edit.Kind == TEXT("static_switch"))
			{
				FStaticSwitchParameter Carrier;
				if (!DecodeValue(Carrier, GET_MEMBER_NAME_CHECKED(FStaticSwitchParameter, Value), Input, OutError)) return false;
				Edit.bSwitch = Carrier.Value;
			}
			else if (Edit.Kind == TEXT("double_vector"))
			{
				FDoubleVectorParameterValue Carrier;
				if (!DecodeValue(Carrier, GET_MEMBER_NAME_CHECKED(FDoubleVectorParameterValue, ParameterValue), Input, OutError)) return false;
				Edit.Native.Value = FMaterialParameterValue(Carrier.ParameterValue);
			}
			else if (Edit.Kind == TEXT("texture_collection"))
			{
				FTextureCollectionParameterValue Carrier;
				if (!DecodeValue(Carrier, GET_MEMBER_NAME_CHECKED(FTextureCollectionParameterValue, ParameterValue), Input, OutError)) return false;
				Edit.Native.Value = FMaterialParameterValue(Carrier.ParameterValue);
				Edit.Resource.Reset(Carrier.ParameterValue.Get());
			}
			else if (Edit.Kind == TEXT("runtime_virtual_texture"))
			{
				FRuntimeVirtualTextureParameterValue Carrier;
				if (!DecodeValue(Carrier, GET_MEMBER_NAME_CHECKED(FRuntimeVirtualTextureParameterValue, ParameterValue), Input, OutError)) return false;
				Edit.Native.Value = FMaterialParameterValue(Carrier.ParameterValue);
				Edit.Resource.Reset(Carrier.ParameterValue.Get());
			}
			else if (Edit.Kind == TEXT("sparse_volume_texture"))
			{
				FSparseVolumeTextureParameterValue Carrier;
				if (!DecodeValue(Carrier, GET_MEMBER_NAME_CHECKED(FSparseVolumeTextureParameterValue, ParameterValue), Input, OutError)) return false;
				Edit.Native.Value = FMaterialParameterValue(Carrier.ParameterValue);
				Edit.Resource.Reset(Carrier.ParameterValue.Get());
			}
			else if (Edit.Kind == TEXT("font"))
			{
				FFontParameterValue Carrier;
				if (!UeAgentPropertyValueCodec::DecodeStructValue(Carrier.StaticStruct(), Input, &Carrier, &Instance, CompoundValueOptions(), OutError)) return false;
				Edit.Native.Value = FMaterialParameterValue(Carrier.FontValue, Carrier.FontPage);
				Edit.Resource.Reset(Carrier.FontValue.Get());
			}
			else if (Edit.Kind == TEXT("static_component_mask"))
			{
				FStaticComponentMaskParameter Carrier;
				if (!UeAgentPropertyValueCodec::DecodeStructValue(Carrier.StaticStruct(), Input, &Carrier, &Instance, CompoundValueOptions(), OutError)) return false;
				Instance.GetParameterValue(EMaterialParameterType::StaticComponentMask, Edit.Info, Edit.Native);
				Edit.Native.Value = FMaterialParameterValue(Carrier.R, Carrier.G, Carrier.B, Carrier.A);
				Edit.Native.bOverride = true;
			}
			else {
				OutError = UnsupportedParameterKind(Edit.Kind, Name);
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Value, TEXT("parameter_kind"));
				return false;
			}
			if (Edit.Native.Value.Type != EMaterialParameterType::None)
			{
				FMaterialParameterMetadata ExistingMetadata;
				if (Instance.GetParameterValue(Edit.Native.Value.Type, Edit.Info, ExistingMetadata))
				{
					ExistingMetadata.Value = Edit.Native.Value;
					Edit.Native = MoveTemp(ExistingMetadata);
				}
				Edit.Native.bOverride = true;
			}
			OutEdits.Add(MoveTemp(Edit));
		}
		return true;
	}

	bool CaptureLayerStack(UObject& Owner, FMaterialLayersFunctions& Layers, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<FJsonValue> LayerValues, BlendValues, Definition;
		if (!CaptureValue(Owner, Layers, TEXT("Layers"), LayerValues, Definition, OutError)
			|| !CaptureValue(Owner, Layers, TEXT("Blends"), BlendValues, Definition, OutError)) return false;
		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<int32> Order;
		TMap<int32, int32> ParentIndices;
		if (!Layers.Tree.IsEmpty())
		{
			TFunction<void(int32)> VisitChildren = [&](int32 ParentNode) {
				for (const int32 NodeId : Layers.GetNodeChildren(ParentNode))
				{
					const int32 LayerIndex = Layers.GetLayerFuncIndex(NodeId);
					Order.Add(LayerIndex);
					ParentIndices.Add(LayerIndex, ParentNode == INDEX_NONE ? INDEX_NONE : Layers.GetLayerFuncIndex(ParentNode));
					VisitChildren(NodeId);
				}
			};
			VisitChildren(INDEX_NONE);
		}
		else for (int32 Index = 0; Index < Layers.Layers.Num(); ++Index) Order.Add(Index);
		for (const int32 Index : Order)
		{
			FString Key;
			UE::UAI::HjsonWorkspace::FWorkspaceError Error;
			if (!UE::UAI::HjsonWorkspace::TryFormatReadableKeyedItemOrdinal(TEXT("Entry"), Index, Key, Error))
			{ OutError = Error.GetDiagnostic(); return false; }
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("instance"), Key);
			if (!Layers.Tree.IsEmpty())
			{
				const int32 Parent = ParentIndices.FindChecked(Index);
				if (Parent == INDEX_NONE) Row->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
				else
				{
					FString ParentKey;
					if (!UE::UAI::HjsonWorkspace::TryFormatReadableKeyedItemOrdinal(TEXT("Entry"), Parent, ParentKey, Error))
					{ OutError = Error.GetDiagnostic(); return false; }
					Row->SetStringField(TEXT("parent"), ParentKey);
				}
			}
			Row->SetStringField(TEXT("name"), Layers.GetLayerName(Index).ToString());
			Row->SetBoolField(TEXT("enabled"), Layers.GetLayerVisibility(Index));
			Row->SetField(TEXT("layer"), LayerValues->AsArray()[Index]);
			const int32 BlendIndex = Layers.Blends.Num() == Layers.Layers.Num() ? Index : Index - 1;
			if (Layers.Blends.IsValidIndex(BlendIndex)) Row->SetField(TEXT("blend"), BlendValues->AsArray()[BlendIndex]);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		auto State = MakeShared<FJsonObject>();
		State->SetArrayField(TEXT("layers"), Rows);
		Value = MakeShared<FJsonValueObject>(State);
		return true;
	}

	bool CaptureInstanceLayers(UMaterialInstanceConstant& Instance, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FMaterialLayersFunctions Layers;
		Instance.GetMaterialLayers(Layers);
		if (!CaptureLayerStack(Instance, Layers, Value, OutError)) return false;
		Value->AsObject()->SetBoolField(TEXT("authored_override"), Instance.GetStaticParameters().bHasMaterialLayers);
		return true;
	}

	bool PrepareLayerStack(const FMaterialLayersFunctions& Before, const TSharedPtr<FJsonValue>& Value,
		FMaterialLayersFunctions& After, UE::UAI::Contract::FDiagnostic& OutError)
	{
		using namespace UE::UAI;
		if (!Value || Value->Type != EJson::Object) {
			OutError = ParameterFailure(TEXT("material_layer_stack_object_required"), TEXT("A material layer stack must be an object."),
				TEXT("instance_layers"), Value, MakeShared<FJsonValueString>(TEXT("object")));
			return false;
		}
		ON_SCOPE_EXIT {
			if (!OutError.IsEmpty()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Value->AsObject());
		};
		After = Before;
		// This derived graph contains transient callers from the previous stack.
		// A copied desired stack must build its own callers after publication.
		After.RuntimeGraphCache.Reset();
		{
			const auto Desired = Value->AsObject()->TryGetField(TEXT("layers"));
			if (!Desired || Desired->Type != EJson::Array) {
				OutError = ParameterFailure(TEXT("material_layer_stack_layers_required"), TEXT("Material layer stack requires a layers array."),
					TEXT("layers"), Desired, MakeShared<FJsonValueString>(TEXT("array")), Value->AsObject().Get());
				return false;
			}
			auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
			TArray<TSharedPtr<FJsonValue>> Definitions;
			for (int32 Index = 0; Index < Before.Layers.Num(); ++Index) Definitions.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
			StateSemantic::FUeAgentCanonicalValuePtr Proposed, Captured;
			if (!StateSemantic::CanonicalJson::TryFromJsonValue(Desired, Resources, Proposed, OutError)
				|| !StateSemantic::CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueArray>(Definitions), Resources, Captured, OutError)) return false;
			TArray<HjsonWorkspace::FOrderedStructuredItemBinding> Bindings;
			HjsonWorkspace::FWorkspaceError Error;
			if (!HjsonWorkspace::TryResolveOrderedStructuredItems(*Proposed, *Captured, Resources, Bindings, Error))
			{
				OutError = Error.GetDiagnostic();
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/layers"));
				return false;
			}
			// The resolver emits one binding for each proposed row, in proposed order.
			// CapturedIndex addresses the old native stack, never the authored document.
			const auto LayerFailure = [&](const TCHAR* Code, const FString& Reason, int32 InputIndex,
				const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected) {
				const auto Input = Desired->AsArray().IsValidIndex(InputIndex) ? Desired->AsArray()[InputIndex] : nullptr;
				const auto InputObject = Input && Input->Type == EJson::Object ? Input->AsObject() : nullptr;
				return ParameterFailure(Code, Reason, Field, Actual, Expected, InputObject.Get());
			};
			// The native background is a fixed first slot. UE's move/remove methods
			// require non-background indices and preserve each surviving layer GUID.
			if (Bindings.IsEmpty() || (Before.Layers.Num() && Bindings[0].CapturedIndex != 0))
			{
				OutError = LayerFailure(TEXT("material_layer_stack_background_required"),
					Bindings.IsEmpty() ? TEXT("A material layer stack must retain a background layer.")
					    : TEXT("The first authored layer must retain the existing background layer Entry0."),
					0, TEXT("instance"), Bindings.IsEmpty() ? nullptr : Desired->AsArray()[0]->AsObject()->TryGetField(TEXT("instance")),
					MakeShared<FJsonValueString>(Before.Layers.Num() ? TEXT("Entry0") : TEXT("background layer")));
				return false;
			}
			TArray<int32> SourceIndices;
			for (const auto& Binding : Bindings) SourceIndices.Add(Binding.CapturedIndex);
			for (int32 Index = Before.Layers.Num() - 1; Index > 0; --Index)
				if (!SourceIndices.Contains(Index)) After.RemoveBlendedLayerAt(Index);
			if (After.IsEmpty()) After.AddDefaultBackgroundLayer();
			for (int32 Index = 1; Index < Bindings.Num(); ++Index)
			{
				const int32 Source = Bindings[Index].CapturedIndex;
				const int32 Current = Source == INDEX_NONE ? After.AppendBlendedLayer()
					: After.EditorOnly.LayerGuids.IndexOfByKey(Before.EditorOnly.LayerGuids[Source]);
				if (Current != Index) After.MoveBlendedLayer(Current, Index);
			}
			TArray<TSharedPtr<FJsonValue>> LayerValues, BlendValues;
			TMap<FString, UE::UAI::Contract::FSourceLocation> DecodeSources;
			TArray<FString> ParentKeys;
			TMap<FString, int32> SourceKeys;
			for (int32 Index = 0; Index < Bindings.Num(); ++Index)
			{
				TSharedPtr<FJsonValue> RowValue;
				if (!StateSemantic::CanonicalJson::TryToJsonValue(*Bindings[Index].Value, RowValue, OutError)) return false;
				const auto Row = RowValue->AsObject();
				const auto InputRow = Desired->AsArray()[Index]->AsObject();
				const auto RowSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*InputRow);
				UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Row.Get(),
					[&RowSources](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(RowSources, Pointer); });
				ON_SCOPE_EXIT {
					if (!OutError.IsEmpty()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *InputRow);
				};
				if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Row,
					{{TEXT("name"), EJson::String}, {TEXT("enabled"), EJson::Boolean}}, OutError)) return false;
				FString ParentKey;
				if (const auto Parent = Row->TryGetField(TEXT("parent")); Parent && Parent->Type != EJson::Null)
				{
					if (!Parent->TryGetString(ParentKey)) {
						OutError = LayerFailure(TEXT("material_layer_parent_reference_invalid"),
							FString::Printf(TEXT("Layer %d parent must be null or an instance key string."), Index),
							Index, TEXT("parent"), Parent, MakeShared<FJsonValueString>(TEXT("null or instance key")));
						return false;
					}
				}
				ParentKeys.Add(ParentKey);
				if (Bindings[Index].CapturedIndex != INDEX_NONE)
				{
					FString SourceKey;
					if (!HjsonWorkspace::TryFormatReadableKeyedItemOrdinal(TEXT("Entry"), Bindings[Index].CapturedIndex, SourceKey, Error))
					{ OutError = Error.GetDiagnostic(); return false; }
					SourceKeys.Add(SourceKey, Index);
				}
				const auto Layer = Row->TryGetField(TEXT("layer"));
				if (!Layer) {
					OutError = LayerFailure(TEXT("material_layer_reference_required"), FString::Printf(TEXT("Layer %d requires a layer function reference."), Index),
						Index, TEXT("layer"), nullptr, MakeShared<FJsonValueString>(TEXT("layer function reference")));
					return false;
				}
				const auto ProjectField = [&](const TCHAR* InputField, const TCHAR* OutputField, int32 OutputIndex) {
					const FString InputPrefix = TEXT("/") + FString(InputField);
					const FString OutputPrefix = FString::Printf(TEXT("/%s/%d"), OutputField, OutputIndex);
					DecodeSources.Add(OutputPrefix, UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(RowSources, InputPrefix));
					for (const auto& Pair : RowSources)
						if (Pair.Key.StartsWith(InputPrefix + TEXT("/")))
							DecodeSources.Add(OutputPrefix + Pair.Key.Mid(InputPrefix.Len()), Pair.Value);
				};
				ProjectField(TEXT("layer"), TEXT("Layers"), LayerValues.Num());
				LayerValues.Add(Layer);
				const int32 BlendIndex = After.Blends.Num() == After.Layers.Num() ? Index : Index - 1;
				if (After.Blends.IsValidIndex(BlendIndex))
				{
					const auto Blend = Row->TryGetField(TEXT("blend"));
					ProjectField(TEXT("blend"), TEXT("Blends"), BlendValues.Num());
					BlendValues.Add(Blend ? Blend : MakeShared<FJsonValueNull>());
				}
				FString Name;
				bool Enabled;
				if (Row->TryGetStringField(TEXT("name"), Name)) After.EditorOnly.LayerNames[Index] = FText::FromString(Name);
				if (Row->TryGetBoolField(TEXT("enabled"), Enabled)) After.SetBlendedLayerVisibility(Index, Enabled);
			}
			auto DecodeDocument = MakeShared<FJsonObject>();
			DecodeDocument->SetArrayField(TEXT("Layers"), LayerValues);
			DecodeDocument->SetArrayField(TEXT("Blends"), BlendValues);
			UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(&DecodeDocument.Get(),
				[&DecodeSources](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(DecodeSources, Pointer); });
			if (!DecodeValue(After, TEXT("Layers"), DecodeDocument->TryGetField(TEXT("Layers")), OutError)
				|| !DecodeValue(After, TEXT("Blends"), DecodeDocument->TryGetField(TEXT("Blends")), OutError)) return false;
			if (!After.Tree.IsEmpty())
			{
				// Payload arrays have been reconciled above. Rebuild only their native
				// tree links, preserving layer GUIDs and using the requested sibling order.
				FMaterialLayersFunctions Topology;
				TArray<int32> NodeIds; NodeIds.Init(INDEX_NONE, Bindings.Num());
				int32 RemainingNodes = Bindings.Num();
				while (RemainingNodes)
				{
					bool Progress = false;
					for (int32 Index = 0; Index < Bindings.Num(); ++Index)
					{
						if (NodeIds[Index] != INDEX_NONE) continue;
						int32 ParentNode = INDEX_NONE;
						if (!ParentKeys[Index].IsEmpty())
						{
							const int32* ParentIndex = SourceKeys.Find(ParentKeys[Index]);
							if (!ParentIndex) {
								OutError = LayerFailure(TEXT("material_layer_parent_unresolved"),
									FString::Printf(TEXT("Layer %d refers to parent '%s', which is not retained in this stack."), Index, *ParentKeys[Index]),
									Index, TEXT("parent"), MakeShared<FJsonValueString>(ParentKeys[Index]), MakeShared<FJsonValueString>(TEXT("retained layer instance key")));
								return false;
							}
							ParentNode = NodeIds[*ParentIndex];
							if (ParentNode == INDEX_NONE) continue;
						}
						if ((Index == 0 && ParentNode != INDEX_NONE) || !Topology.CanAppendLayerNode(ParentNode))
						{
							OutError = LayerFailure(TEXT("material_layer_parent_not_supported_by_native_tree"),
								FString::Printf(TEXT("Layer %d cannot use parent '%s': %s."), Index, *ParentKeys[Index],
									Index == 0 && ParentNode != INDEX_NONE ? TEXT("the background must remain a root") : TEXT("UE rejected this parent relationship")),
								Index, TEXT("parent"), MakeShared<FJsonValueString>(ParentKeys[Index]),
								MakeShared<FJsonValueString>(Index == 0 ? TEXT("root") : TEXT("parent accepted by the native layer tree")));
							return false;
						}
						NodeIds[Index] = Topology.AppendLayerNode(ParentNode);
						Topology.Tree.Payloads[NodeIds[Index]] = {Index, Index};
						--RemainingNodes; Progress = true;
					}
					if (!Progress) {
						const int32 Pending = NodeIds.IndexOfByKey(INDEX_NONE);
						OutError = LayerFailure(TEXT("material_layer_parent_cycle"),
							FString::Printf(TEXT("The layer parent chain cannot be resolved: layer %d still depends on '%s' after no pending layer could advance."), Pending, *ParentKeys[Pending]),
							Pending, TEXT("parent"), MakeShared<FJsonValueString>(ParentKeys[Pending]), MakeShared<FJsonValueString>(TEXT("acyclic layer parent chain")));
						return false;
					}
				}
				After.Tree = MoveTemp(Topology.Tree);
			}
			for (int32 Index = 0; Index < After.Layers.Num(); ++Index)
			{
				const int32 Source = Bindings[Index].CapturedIndex;
				if (Source == INDEX_NONE) continue;
				const int32 Blend = After.Blends.Num() == After.Layers.Num() ? Index : Index - 1;
				const int32 OldBlend = Before.Blends.Num() == Before.Layers.Num() ? Source : Source - 1;
				// ResolveParent copies linked layer names from the parent as well as
				// function references. A local rename must therefore unlink the row.
				if (!Before.EditorOnly.LayerNames[Source].EqualTo(After.EditorOnly.LayerNames[Index])
					|| Before.Layers[Source] != After.Layers[Index]
					|| (After.Blends.IsValidIndex(Blend) && Before.Blends.IsValidIndex(OldBlend) && Before.Blends[OldBlend] != After.Blends[Blend]))
					After.UnlinkLayerFromParent(Index);
			}
		}
		return true;
	}

	bool ApplyInstanceLayers(UMaterialInstanceConstant& Instance, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
	{
		bool Override = false;
		if (!Value || Value->Type != EJson::Object || !Value->AsObject()->TryGetBoolField(TEXT("authored_override"), Override))
		{
			const auto Input = Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			OutError = ParameterFailure(TEXT("material_layer_stack_override_required"), TEXT("Material layer stack requires a boolean authored_override field."),
				TEXT("authored_override"), Input ? Input->TryGetField(TEXT("authored_override")) : Value,
				MakeShared<FJsonValueString>(TEXT("boolean")), Input.Get());
			return false;
		}
		FMaterialLayersFunctions Before, After;
		Instance.GetMaterialLayers(Before);
		if (Override)
		{
			if (!PrepareLayerStack(Before, Value, After, OutError)) return false;
		}
		else if (Instance.Parent) Instance.Parent->GetMaterialLayers(After);
		Instance.Modify();
		// Match the native editor: update parameter associations together with
		// structural removal/swaps, before publishing the new layer stack.
		auto Remaining = Before.EditorOnly.LayerGuids;
		if (Remaining.IsEmpty() && !After.EditorOnly.LayerGuids.IsEmpty()) Remaining.Add(After.EditorOnly.LayerGuids[0]);
		for (int32 Index = Remaining.Num() - 1; Index >= (After.IsEmpty() ? 0 : 1); --Index)
			if (!After.EditorOnly.LayerGuids.Contains(Remaining[Index]))
			{ Instance.RemoveLayerParameterIndex(Index); Remaining.RemoveAt(Index); }
		for (int32 Index = 1; Index < After.EditorOnly.LayerGuids.Num(); ++Index)
		{
			int32 Current = Remaining.IndexOfByKey(After.EditorOnly.LayerGuids[Index]);
			if (Current == INDEX_NONE) Current = Remaining.Add(After.EditorOnly.LayerGuids[Index]);
			if (Current != Index) { Instance.SwapLayerParameterIndices(Current, Index); Remaining.Swap(Current, Index); }
		}
		if (Override) Instance.SetMaterialLayers(After);
		else
		{
			FMaterialInstanceParameterUpdateContext Update(&Instance);
			auto& Parameters = Update.GetStaticParameters();
			Parameters.bHasMaterialLayers = false;
			Parameters.MaterialLayers.Empty();
			Parameters.EditorOnly.MaterialLayers.Empty();
		}
		UMaterialEditingLibrary::UpdateMaterialInstance(&Instance);
		return true;
	}

	namespace
	{
	template <typename T>
	bool VerifyParameterArrays(T& Instance, const TArray<FStaticSwitchParameter>& Switches,
		const TArray<FStaticComponentMaskParameter>& Masks,
		const TArray<FInstanceParameterEdit>& Edits, UE::UAI::Contract::FDiagnostic& OutError)
	{
		for (const auto& Edit : Edits)
		{
			UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Edit.Source.Get(),
				[&Edit](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Edit.SourceLocations, Pointer); });
			ON_SCOPE_EXIT {
				if (!OutError.IsEmpty() && Edit.Source) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Edit.Source);
			};
			// Preparation already decoded the request through the shared property
			// codec. Compare native parameter carriers, not another post-write export.
			FMaterialParameterValue Expected = Edit.Native.Value;
			if (Edit.Kind == TEXT("scalar")) Expected = FMaterialParameterValue(Edit.Scalar);
			else if (Edit.Kind == TEXT("vector")) Expected = FMaterialParameterValue(Edit.Vector);
			else if (Edit.Kind == TEXT("texture")) Expected = FMaterialParameterValue(Edit.Texture.Get());
			else if (Edit.Kind == TEXT("static_switch")) Expected = FMaterialParameterValue(Edit.bSwitch);
			UE::UAI::StateSemantic::CanonicalJson::FValueComparison Difference;
			UE::UAI::Contract::FDiagnostic ProjectionError;
			auto Matches = [&Edit, &Expected, &Instance, &Difference, &ProjectionError](const auto& Parameters) {
				const auto* Parameter = Parameters.FindByPredicate([&Edit](const auto& P) { return P.ParameterInfo == Edit.Info; });
				const bool bLocal = Parameter && Parameter->IsOverride();
				if (!Edit.bOverride || !bLocal) {
					if (bLocal != Edit.bOverride) Difference = UE::UAI::StateSemantic::CanonicalJson::FValueComparison::Different(
						TEXT("/authored_override"), MakeShared<FJsonValueBoolean>(Edit.bOverride), MakeShared<FJsonValueBoolean>(bLocal));
					return !Edit.bOverride && !bLocal;
				}
				FMaterialParameterMetadata Actual;
				Parameter->GetValue(Actual);
				if (Actual.Value == Expected) return true;
				auto ActualCarrier = *Parameter;
				auto ExpectedCarrier = *Parameter;
				using TCarrier = decltype(ExpectedCarrier);
				if constexpr (std::is_same_v<TCarrier, FScalarParameterValue>) ExpectedCarrier.ParameterValue = Edit.Scalar;
				else if constexpr (std::is_same_v<TCarrier, FVectorParameterValue>) ExpectedCarrier.ParameterValue = Edit.Vector;
				else if constexpr (std::is_same_v<TCarrier, FTextureParameterValue>) ExpectedCarrier.ParameterValue = Edit.Texture.Get();
				else if constexpr (std::is_same_v<TCarrier, FStaticSwitchParameter>) ExpectedCarrier.Value = Edit.bSwitch;
				else if constexpr (std::is_same_v<TCarrier, FDoubleVectorParameterValue>) ExpectedCarrier.ParameterValue = Edit.Native.Value.AsVector4d();
				else if constexpr (std::is_same_v<TCarrier, FTextureCollectionParameterValue>) ExpectedCarrier.ParameterValue = Edit.Native.Value.TextureCollection;
				else if constexpr (std::is_same_v<TCarrier, FRuntimeVirtualTextureParameterValue>) ExpectedCarrier.ParameterValue = Edit.Native.Value.RuntimeVirtualTexture;
				else if constexpr (std::is_same_v<TCarrier, FSparseVolumeTextureParameterValue>) ExpectedCarrier.ParameterValue = Edit.Native.Value.SparseVolumeTexture;
				else if constexpr (std::is_same_v<TCarrier, FFontParameterValue>) {
					ExpectedCarrier.FontValue = Edit.Native.Value.Font.Value;
					ExpectedCarrier.FontPage = Edit.Native.Value.Font.Page;
				}
				else if constexpr (std::is_same_v<TCarrier, FStaticComponentMaskParameter>) {
					const auto Mask = Edit.Native.Value.AsStaticComponentMask();
					ExpectedCarrier.R = Mask.R;
					ExpectedCarrier.G = Mask.G;
					ExpectedCarrier.B = Mask.B;
					ExpectedCarrier.A = Mask.A;
				}
				TSharedPtr<FJsonValue> ExpectedJson, ActualJson, Definition;
				bool bProjected;
				if constexpr (std::is_same_v<TCarrier, FFontParameterValue> || std::is_same_v<TCarrier, FStaticComponentMaskParameter>)
					bProjected = CaptureCompound(Instance, ExpectedCarrier, ExpectedJson, Definition, ProjectionError)
						&& CaptureCompound(Instance, ActualCarrier, ActualJson, Definition, ProjectionError);
				else {
					const FName Field = std::is_same_v<TCarrier, FStaticSwitchParameter> ? TEXT("Value") : TEXT("ParameterValue");
					bProjected = CaptureValue(Instance, ExpectedCarrier, Field, ExpectedJson, Definition, ProjectionError)
						&& CaptureValue(Instance, ActualCarrier, Field, ActualJson, Definition, ProjectionError);
				}
				if (bProjected) Difference = UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(ExpectedJson, ActualJson).WithPrefix(TEXT("/value"));
				return false;
			};
			bool bMatches = false;
			if (Edit.Kind == TEXT("scalar")) bMatches = Matches(Instance.ScalarParameterValues);
			else if (Edit.Kind == TEXT("vector")) bMatches = Matches(Instance.VectorParameterValues);
			else if (Edit.Kind == TEXT("texture")) bMatches = Matches(Instance.TextureParameterValues);
			else if (Edit.Kind == TEXT("double_vector")) bMatches = Matches(Instance.DoubleVectorParameterValues);
			else if (Edit.Kind == TEXT("texture_collection")) bMatches = Matches(Instance.TextureCollectionParameterValues);
			else if (Edit.Kind == TEXT("runtime_virtual_texture")) bMatches = Matches(Instance.RuntimeVirtualTextureParameterValues);
			else if (Edit.Kind == TEXT("sparse_volume_texture")) bMatches = Matches(Instance.SparseVolumeTextureParameterValues);
			else if (Edit.Kind == TEXT("font")) bMatches = Matches(Instance.FontParameterValues);
			else if (Edit.Kind == TEXT("static_switch")) bMatches = Matches(Switches);
			else if (Edit.Kind == TEXT("static_component_mask")) bMatches = Matches(Masks);
			if (!bMatches) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_instance_parameter_intent_mismatch"),
                    FString::Printf(TEXT("Material parameter '%s' (%s, association %d, index %d) did not retain its requested value or override state."),
                        *Edit.Info.Name.ToString(), *Edit.Kind, static_cast<int32>(Edit.Info.Association), Edit.Info.Index), TEXT("readback"));
                OutError.TargetPath = Instance.GetPathName();
                OutError.PropertyPath = Edit.Info.Name.ToString();
				if (!Difference.bEquivalent) Difference.ApplyTo(OutError);
				else {
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					OutError.PropertyPath = TEXT("native_parameter_equivalent");
					if (!ProjectionError.IsEmpty()) OutError.Append(ProjectionError);
				}
				return false;
			}
		}
		return true;
	}
	}

	bool VerifyInstanceParameters(UMaterialInstanceConstant& Instance,
		const TArray<FInstanceParameterEdit>& Edits, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FStaticParameterSet Static = Instance.GetStaticParameters();
		return VerifyParameterArrays(Instance, Static.StaticSwitchParameters, Static.EditorOnly.StaticComponentMaskParameters, Edits, OutError);
	}

	bool VerifyInstanceParameters(UMaterialFunctionInstance& Instance,
		const TArray<FInstanceParameterEdit>& Edits, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return VerifyParameterArrays(Instance, Instance.StaticSwitchParameterValues, Instance.StaticComponentMaskParameterValues, Edits, OutError);
	}

	bool ApplyInstanceParameters(UMaterialInstanceConstant& Instance,
		const TArray<FInstanceParameterEdit>& Edits, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FMaterialNiagaraGraphNativeHook Hook;
		for (const auto& Edit : Edits)
		{
			UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Edit.Source.Get(),
				[&Edit](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Edit.SourceLocations, Pointer); });
			ON_SCOPE_EXIT {
				if (!OutError.IsEmpty() && Edit.Source) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Edit.Source);
			};
			if (!Edit.bOverride)
			{
				Instance.Modify();
				const auto Matches = [&Edit](const auto& Parameter) { return Parameter.ParameterInfo == Edit.Info; };
				if (Edit.Kind == TEXT("scalar")) Instance.ScalarParameterValues.RemoveAll(Matches);
				else if (Edit.Kind == TEXT("vector")) Instance.VectorParameterValues.RemoveAll(Matches);
				else if (Edit.Kind == TEXT("texture")) Instance.TextureParameterValues.RemoveAll(Matches);
				else if (Edit.Kind == TEXT("double_vector")) Instance.DoubleVectorParameterValues.RemoveAll(Matches);
				else if (Edit.Kind == TEXT("texture_collection")) Instance.TextureCollectionParameterValues.RemoveAll(Matches);
				else if (Edit.Kind == TEXT("runtime_virtual_texture")) Instance.RuntimeVirtualTextureParameterValues.RemoveAll(Matches);
				else if (Edit.Kind == TEXT("sparse_volume_texture")) Instance.SparseVolumeTextureParameterValues.RemoveAll(Matches);
				else if (Edit.Kind == TEXT("font")) Instance.FontParameterValues.RemoveAll(Matches);
				else if (Edit.Kind == TEXT("static_component_mask"))
				{
					FMaterialInstanceParameterUpdateContext Update(&Instance);
					Update.GetStaticParameters().EditorOnly.StaticComponentMaskParameters.RemoveAll(Matches);
				}
				else if (Edit.Kind == TEXT("static_switch"))
				{
					FMaterialInstanceParameterUpdateContext Update(&Instance);
					Update.GetStaticParameters().StaticSwitchParameters.RemoveAll(Matches);
				}
				else { OutError = UnsupportedParameterKind(Edit.Kind, Edit.Info.Name.ToString()); return false; }
				UMaterialEditingLibrary::UpdateMaterialInstance(&Instance);
				continue;
			}
			if (Edit.Kind == TEXT("texture") && !Edit.Texture.IsValid())
			{
				Instance.Modify();
				auto* Parameter = Instance.TextureParameterValues.FindByPredicate(
					[&Edit](const auto& Candidate) { return Candidate.ParameterInfo == Edit.Info; });
				if (!Parameter)
				{
					Parameter = &Instance.TextureParameterValues.AddDefaulted_GetRef();
					Parameter->ParameterInfo = Edit.Info;
				}
				Parameter->ParameterValue = nullptr;
				UMaterialEditingLibrary::UpdateMaterialInstance(&Instance);
				continue;
			}
			if (Edit.Native.Value.Type != EMaterialParameterType::None)
			{
				Instance.Modify();
				auto SetLocal = [&Edit](auto& Parameters, auto SetValue) {
					auto* Parameter = Parameters.FindByPredicate([&Edit](const auto& P) { return P.ParameterInfo == Edit.Info; });
					if (!Parameter) {
						Parameter = &Parameters.AddDefaulted_GetRef();
						Parameter->ParameterInfo = Edit.Info;
						Parameter->ExpressionGUID = Edit.Native.ExpressionGuid;
					}
					SetValue(*Parameter);
				};
				switch (Edit.Native.Value.Type)
				{
				case EMaterialParameterType::DoubleVector:
					SetLocal(Instance.DoubleVectorParameterValues, [&Edit](auto& P) { P.ParameterValue = Edit.Native.Value.AsVector4d(); }); break;
				case EMaterialParameterType::TextureCollection:
					SetLocal(Instance.TextureCollectionParameterValues, [&Edit](auto& P) { P.ParameterValue = Edit.Native.Value.TextureCollection; }); break;
				case EMaterialParameterType::RuntimeVirtualTexture:
					SetLocal(Instance.RuntimeVirtualTextureParameterValues, [&Edit](auto& P) { P.ParameterValue = Edit.Native.Value.RuntimeVirtualTexture; }); break;
				case EMaterialParameterType::SparseVolumeTexture:
					SetLocal(Instance.SparseVolumeTextureParameterValues, [&Edit](auto& P) { P.ParameterValue = Edit.Native.Value.SparseVolumeTexture; }); break;
				case EMaterialParameterType::Font:
					SetLocal(Instance.FontParameterValues, [&Edit](auto& P) { P.FontValue = Edit.Native.Value.Font.Value; P.FontPage = Edit.Native.Value.Font.Page; }); break;
				case EMaterialParameterType::StaticComponentMask:
				{
					FMaterialInstanceParameterUpdateContext Update(&Instance);
					Update.SetParameterValueEditorOnly(Edit.Info, Edit.Native);
					break;
				}
				default: OutError = UnsupportedParameterKind(Edit.Kind, Edit.Info.Name.ToString()); return false;
				}
				UMaterialEditingLibrary::UpdateMaterialInstance(&Instance);
				continue;
			}
			FMaterialNiagaraGraphNativeMutationReceipt Receipt;
			FMaterialNiagaraGraphAdapterError Error;
			bool bApplied = false;
			if (Edit.Kind == TEXT("scalar")) bApplied = Hook.TrySetMaterialInstanceScalarParameter(Instance, Edit.Info, Edit.Scalar, Receipt, Error);
			else if (Edit.Kind == TEXT("vector")) bApplied = Hook.TrySetMaterialInstanceVectorParameter(Instance, Edit.Info, Edit.Vector, Receipt, Error);
			else if (Edit.Kind == TEXT("texture"))
			{
				UTexture* Texture = Edit.Texture.Get();
				bApplied = Texture && Hook.TrySetMaterialInstanceTextureParameter(Instance, Edit.Info, *Texture, Receipt, Error);
			}
			else if (Edit.Kind == TEXT("static_switch")) bApplied = Hook.TrySetMaterialInstanceStaticSwitchParameter(Instance, Edit.Info, Edit.bSwitch, Receipt, Error);
			if (!bApplied) {
				OutError = Error.IsEmpty() ? ParameterFailure(TEXT("material_instance_parameter_apply_failed"),
					FString::Printf(TEXT("UE could not apply parameter '%s' (%s, association %d, index %d)."),
						*Edit.Info.Name.ToString(), *Edit.Kind, static_cast<int32>(Edit.Info.Association), Edit.Info.Index),
					TEXT("parameter_applied"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)) : Error;
				return false;
			}
		}
		return VerifyInstanceParameters(Instance, Edits, OutError);
	}
}
