#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Domains/Niagara/UeAgentNiagaraEmitterCompileContext.h"
#include "Domains/Niagara/UeAgentNiagaraStackValidation.h"
#include "Engine/Texture.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph
{
	namespace
	{
		TSharedPtr<FJsonValue> FiniteNumberFact(double Value)
		{
			return FMath::IsFinite(Value) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(::LexToString(Value)));
		}

		FMaterialNiagaraGraphAdapterError NativeValueFailure(const FString& Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
			const UObject* NativeOwner = nullptr)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_adapter"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			if (NativeOwner) Error.SubjectId = NativeOwner->GetPathName();
			return Error;
		}

		FMaterialNiagaraGraphAdapterError NativeConditionFailure(const FString& Code, const FString& Reason,
			const FString& Field, const UObject* NativeOwner = nullptr, bool Actual = false, bool Expected = true)
		{
			return NativeValueFailure(Code, Reason, Field, MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected), NativeOwner);
		}
	}


	namespace
	{
		FMaterialNiagaraGraphAdapterError ParameterReadFailure(const FMaterialNiagaraGraphParameterIdentity& Identity,
			const FString& Type, const FString& Reason, const UObject* NativeOwner,
			const FString& Condition = TEXT("value_read_succeeded"))
		{
			const FString Name = Identity.GetParameterName().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			auto Error = NativeConditionFailure(TEXT("material_niagara_graph_parameter_capture_failed"), Reason,
				TEXT("/parameters/") + Type + TEXT("/") + Identity.GetParameterAssociation() + TEXT("/")
					+ FString::FromInt(Identity.GetParameterIndex()) + TEXT("/") + Name + TEXT("/") + Condition, NativeOwner);
			Error.CandidateName = Identity.GetParameterName();
			return Error;
		}


	}

	FMaterialNiagaraGraphEffectiveParameter::
	FMaterialNiagaraGraphEffectiveParameter(
		FMaterialNiagaraGraphParameterIdentity InIdentity,
		const bool bInOverridden,
		const float InScalarValue,
		FLinearColor InVectorValue,
		FString InTextureRef,
		const bool bInBooleanValue)
		: Identity(MoveTemp(InIdentity))
		, bOverridden(bInOverridden)
		, ScalarValue(InScalarValue)
		, VectorValue(MoveTemp(InVectorValue))
		, TextureRef(MoveTemp(InTextureRef))
		, bBooleanValue(bInBooleanValue)
	{
		FMaterialNiagaraGraphAdapterError ValidationError;
		const bool bScalarFinite = FMath::IsFinite(ScalarValue);
		bValid = bScalarFinite && TryValidateMaterialNiagaraGraphParameterIdentities({Identity}, ValidationError);
		if (!bScalarFinite)
		{
			ValidationError = NativeValueFailure(TEXT("material_niagara_graph_parameter_capture_failed"),
				TEXT("The captured material scalar value must be finite."), TEXT("/scalar_value"),
				FiniteNumberFact(ScalarValue), MakeShared<FJsonValueString>(TEXT("finite number")));
			ValidationError.CandidateName = Identity.GetParameterName();
		}
		else if (!bValid)
		{
			// The identity validator received one local element, not an index in the outer capture.
			const FString LocalIdentityRoot = TEXT("/parameters/0");
			ValidationError.PropertyPath = TEXT("/identity") + ValidationError.PropertyPath.RightChop(LocalIdentityRoot.Len());
		}
		if (!bValid) ValidationDiagnostic = MakeShared<FMaterialNiagaraGraphAdapterError>(MoveTemp(ValidationError));
	}

	const FMaterialNiagaraGraphParameterIdentity&
	FMaterialNiagaraGraphEffectiveParameter::GetIdentity() const
	{
		return Identity;
	}

	bool FMaterialNiagaraGraphEffectiveParameter::IsOverridden() const
	{
		return bOverridden;
	}

	float FMaterialNiagaraGraphEffectiveParameter::GetScalarValue() const
	{
		return ScalarValue;
	}

	const FLinearColor&
	FMaterialNiagaraGraphEffectiveParameter::GetVectorValue() const
	{
		return VectorValue;
	}

	const FString&
	FMaterialNiagaraGraphEffectiveParameter::GetTextureRef() const
	{
		return TextureRef;
	}

	bool FMaterialNiagaraGraphEffectiveParameter::GetBooleanValue() const
	{
		return bBooleanValue;
	}

	bool FMaterialNiagaraGraphEffectiveParameter::IsValid() const
	{
		return bValid;
	}

	namespace
	{
		using namespace UeAgentNiagaraStageModuleAuthoring;

		FString NewEvidence(const TCHAR* Stage, const UObject* Subject)
		{
			return FString::Printf(
				TEXT("native:%s:%s:%s"),
				Stage,
				Subject ? *Subject->GetPathName() : TEXT("none"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}

		FString GuidText(const FGuid& Guid)
		{
			return Guid.IsValid()
				? Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
				: FString();
		}

		FString ExpressionItemKey(const UMaterialExpression& Expression)
		{
			return Expression.MaterialExpressionGuid.IsValid()
				? TEXT("expression:")
					+ GuidText(Expression.MaterialExpressionGuid)
				: FString();
		}

		FString OutputPinKey(
			const UMaterialExpression& Expression,
			const int32 OutputIndex)
		{
			return FString::Printf(
				TEXT("pin:%s:output:%d"),
				*ExpressionItemKey(Expression),
				OutputIndex);
		}

		FString InputPinKey(
			const UMaterialExpression& Expression,
			const int32 InputIndex,
			const FString& InputName)
		{
			return FString::Printf(
				TEXT("pin:%s:input:%d:%s"),
				*ExpressionItemKey(Expression),
				InputIndex,
				*InputName.ToLower());
		}

		FString MaterialPropertyToken(const EMaterialProperty Property)
		{
			const FString NativeName =
				UeAgentMaterialAuthoring::MaterialPropertyToString(Property);
			FString Token;
			Token.Reserve(NativeName.Len() + 4);
			for (int32 Index = 0; Index < NativeName.Len(); ++Index)
			{
				const TCHAR Character = NativeName[Index];
				if (FChar::IsUpper(Character)
					&& Index > 0
					&& Token.Len() > 0
					&& Token[Token.Len() - 1] != TEXT('_'))
				{
					Token.AppendChar(TEXT('_'));
				}
				Token.AppendChar(
					FChar::IsWhitespace(Character)
						? TEXT('_')
						: FChar::ToLower(Character));
			}
			return Token;
		}

		FString ParameterAssociationToken(
			const EMaterialParameterAssociation Association)
		{
			switch (Association)
			{
			case EMaterialParameterAssociation::GlobalParameter:
				return TEXT("global");
			case EMaterialParameterAssociation::LayerParameter:
				return TEXT("layer");
			case EMaterialParameterAssociation::BlendParameter:
				return TEXT("blend");
			}
			return FString();
		}

		FMaterialNiagaraGraphParameterIdentity ParameterIdentity(
			const FMaterialParameterInfo& Info,
			const EMaterialNiagaraGraphParameterType Type)
		{
			return FMaterialNiagaraGraphParameterIdentity(
				Info.Name.ToString(),
				ParameterAssociationToken(Info.Association),
				Info.Index,
				Type);
		}

		FString RootPinKey(const EMaterialProperty Property)
		{
			return TEXT("pin:material:root:")
				+ MaterialPropertyToken(Property);
		}

		FString ModuleItemKey(const UNiagaraNodeFunctionCall& Module)
		{
			return Module.NodeGuid.IsValid()
				? TEXT("module:") + GuidText(Module.NodeGuid)
				: FString();
		}

		class FTopologyHasher final
		{
		public:
			void Mix(const FString& Value)
			{
				const FTCHARToUTF8 Utf8(*Value);
				for (int32 Index = 0; Index < Utf8.Length(); ++Index)
				{
					Hash ^= static_cast<uint8>(Utf8.Get()[Index]);
					Hash *= 1099511628211ull;
				}
				Hash ^= 0xff;
				Hash *= 1099511628211ull;
			}

			FString Revision() const
			{
				return FString::Printf(
					TEXT("native-revision:%016llx"), Hash);
			}

		private:
			uint64 Hash = 1469598103934665603ull;
		};

		FString TopologyRevision(
			const FMaterialNiagaraGraphTopology& Topology)
		{
			TArray<FString> Facts;
			Facts.Add(Topology.GetTargetKind());
			Facts.Add(Topology.GetTargetIdentity());
			Facts.Add(Topology.GetStructureIdentity());
			for (const FMaterialNiagaraGraphItemIdentity& Item
				: Topology.GetItems())
			{
				Facts.Add(TEXT("item|") + Item.GetItemKey() + TEXT("|")
					+ Item.GetNativeGuid() + TEXT("|")
					+ Item.GetStableBuiltinRole());
			}
			for (const FMaterialNiagaraGraphPinIdentity& Pin
				: Topology.GetPins())
			{
				Facts.Add(TEXT("pin|") + Pin.GetPinKey() + TEXT("|")
					+ Pin.GetOwnerItemKey() + TEXT("|")
					+ Pin.GetNativeName()
					+ FString::Printf(TEXT("|%d"), Pin.GetNativeIndex())
					+ (Pin.GetDirection()
						== EMaterialNiagaraGraphPinDirection::Output
							? TEXT("|out") : TEXT("|in")));
			}
			for (const FMaterialNiagaraGraphLinkIdentity& Link
				: Topology.GetLinks())
			{
				Facts.Add(TEXT("link|") + Link.GetLinkKey() + TEXT("|")
					+ Link.GetFromPinKey() + TEXT("|")
					+ Link.GetToPinKey());
			}
			for (const FMaterialNiagaraGraphOrderingIdentity& Ordering
				: Topology.GetOrdering())
			{
				Facts.Add(TEXT("order|") + Ordering.GetOrderingKey()
					+ TEXT("|") + Ordering.GetItemKey() + TEXT("|")
					+ Ordering.GetPredecessorItemKey());
			}
			Facts.Sort();
			FTopologyHasher Hasher;
			for (const FString& Fact : Facts)
			{
				Hasher.Mix(Fact);
			}
			return Hasher.Revision();
		}

		FString ParameterRevision(TArray<FString> Facts)
		{
			Facts.Sort();
			FTopologyHasher Hasher;
			for (const FString& Fact : Facts)
			{
				Hasher.Mix(Fact);
			}
			return Hasher.Revision();
		}

		void SetMutationReceipt(
			const UObject& Subject,
			FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt)
		{
			OutReceipt.SetEvidenceId(NewEvidence(TEXT("apply"), &Subject));
		}

		FString MaterialInputName(
			UMaterialExpression& Expression,
			const int32 InputIndex,
			const FExpressionInput& Input)
		{
			FString Name;
			if (const UMaterialExpressionMaterialFunctionCall* FunctionCall =
				Cast<UMaterialExpressionMaterialFunctionCall>(&Expression))
			{
				Name = FunctionCall->GetInputNameWithType(
					InputIndex, false).ToString();
			}
			else
			{
				Name = Expression.GetInputName(InputIndex).ToString();
			}
			return Name.IsEmpty() ? Input.InputName.ToString() : Name;
		}

		bool TryResolveStage(
			UObject& Asset,
			const FGuid& EmitterHandleId,
			const ENiagaraScriptUsage Usage,
			const FGuid& UsageId,
			FResolvedStage& OutStage,
			FMaterialNiagaraGraphAdapterError& OutError)
		{
			TArray<FResolvedStage> Stages;
			UE::UAI::Contract::FDiagnostic NativeError;
			if (!EnumerateStages(Asset, Stages, NativeError))
			{
				OutError = NativeError;
				return false;
			}
			TArray<FResolvedStage> Matches;
			for (const FResolvedStage& Stage : Stages)
			{
				if (Stage.Usage != Usage || Stage.UsageId != UsageId)
				{
					continue;
				}
				if (EmitterHandleId.IsValid()
					&& Stage.EmitterHandleId != EmitterHandleId)
				{
					continue;
				}
				if (!EmitterHandleId.IsValid()
					&& Stage.EmitterHandleId.IsValid())
				{
					continue;
				}
				Matches.Add(Stage);
			}
			if (Matches.Num() != 1)
			{
				OutError = NativeValueFailure(Matches.IsEmpty() ? TEXT("material_niagara_graph_stage_not_found")
					: TEXT("material_niagara_graph_stage_ambiguous"),
					Matches.IsEmpty() ? TEXT("No Niagara stage matches the requested script usage and native stage/emitter selectors.")
						: TEXT("More than one Niagara stage matches the requested script usage and native stage/emitter selectors."),
					TEXT("/stage_match_count"), MakeShared<FJsonValueNumber>(Matches.Num()), MakeShared<FJsonValueNumber>(1), &Asset);
				OutError.CandidateName = ScriptUsageToString(Usage);
				const auto Selectors = MakeShared<FJsonObject>();
				Selectors->SetStringField(TEXT("usage_id"), GuidText(UsageId));
				Selectors->SetStringField(TEXT("emitter_handle_id"), GuidText(EmitterHandleId));
				OutError.ActualEvidence = MakeShared<FJsonValueObject>(Selectors);
				return false;
			}
			OutStage = Matches[0];
			return true;
		}

		int32 SettleMaterialCompileErrors(UMaterialInterface& Material, TArray<FMaterialNiagaraGraphAdapterError>& OutErrors)
		{
			int32 ErrorCount = 0;
			TSet<FMaterialResource*> Seen;
			TSet<FString> ReportedMessages;
			UMaterialInterface::IterateOverActiveFeatureLevels(
				[&](const ERHIFeatureLevel::Type FeatureLevel)
				{
					FMaterialResource* Resource = Material.GetMaterialResource(FeatureLevel);
					if (Resource && !Seen.Contains(Resource))
					{
						Seen.Add(Resource);
						Resource->FinishCompilation();
						ErrorCount += Resource->GetCompileErrors().Num();
						for (const FString& Message : Resource->GetCompileErrors())
						{
							if (ReportedMessages.Contains(Message)) continue;
							ReportedMessages.Add(Message);
							auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_shader_compile_error"),
								Message.IsEmpty() ? TEXT("The native material compiler returned an empty error message.") : Message, TEXT("material_compile"));
							Error.SourceKind = TEXT("native_result");
							Error.PropertyPath = TEXT("/compile_errors/") + FString::FromInt(OutErrors.Num());
							Error.SubjectId = Material.GetPathName();
							OutErrors.Add(MoveTemp(Error));
						}
					}
				});
			return ErrorCount;
		}
	}

	bool FMaterialNiagaraGraphNativeHook::TryAddMaterialExpression(
		UObject& Owner,
		UClass& ExpressionClass,
		const FGuid& ExpressionGuid,
		const int32 PositionX,
		const int32 PositionY,
		UMaterialExpression*& OutExpression,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutExpression = nullptr;
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		if (!ExpressionGuid.IsValid())
		{
			OutError = NativeConditionFailure(TEXT("material_niagara_graph_expression_guid_invalid"),
				TEXT("The new material expression requires a valid native identity."), TEXT("/expression/native_identity_valid"), &Owner);
			OutError.ActualEvidence = MakeShared<FJsonValueString>(GuidText(ExpressionGuid));
			return false;
		}
		if (!ExpressionClass.IsChildOf(UMaterialExpression::StaticClass())
			|| ExpressionClass.HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = NativeValueFailure(TEXT("material_niagara_graph_expression_class_invalid"),
				!ExpressionClass.IsChildOf(UMaterialExpression::StaticClass())
					? TEXT("The requested class is not a material expression class.") : TEXT("An abstract material expression class cannot be instantiated."),
				TEXT("/expression/class"), MakeShared<FJsonValueString>(ExpressionClass.GetPathName()),
				MakeShared<FJsonValueString>(TEXT("nonabstract UMaterialExpression subclass")), &Owner);
			return false;
		}
		const FMaterialExpressionCollection* Collection =
			UeAgentMaterialAuthoring::GetExpressionCollection(Owner);
		if (!Collection)
		{
			OutError = NativeValueFailure(TEXT("material_niagara_graph_owner_type_invalid"),
				TEXT("The native owner has no editable material expression collection."), TEXT("/owner_class"),
				MakeShared<FJsonValueString>(Owner.GetClass()->GetPathName()),
				MakeShared<FJsonValueString>(TEXT("material or material function graph owner")), &Owner);
			return false;
		}
		for (UMaterialExpression* Existing : Collection->Expressions)
		{
			if (Existing
				&& Existing->MaterialExpressionGuid == ExpressionGuid)
			{
				OutError = NativeConditionFailure(TEXT("material_niagara_graph_item_identity_duplicate"),
					TEXT("The requested native expression identity is already used by an existing expression."),
					TEXT("/expression/native_identity_unique"), &Owner);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(GuidText(ExpressionGuid));
				OutError.CandidateName = Existing->GetClass()->GetName();
				return false;
			}
		}

		Owner.Modify();
		OutExpression = UeAgentMaterialAuthoring::CreateExpression(
			Owner, &ExpressionClass, PositionX, PositionY);
		if (!OutExpression)
		{
			OutError = NativeConditionFailure(TEXT("material_niagara_graph_expression_create_failed"),
				TEXT("Unreal did not create the requested material expression."), TEXT("/expression/created"), &Owner);
			OutError.CandidateName = ExpressionClass.GetPathName();
			return false;
		}
		OutExpression->Modify();
		OutExpression->MaterialExpressionGuid = ExpressionGuid;
		UeAgentMaterialAuthoring::RefreshGraphOwner(Owner);
		SetMutationReceipt(Owner, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TryConnectMaterialExpressionToProperty(
		UMaterialExpression& Expression,
		const FString& OutputName,
		const EMaterialProperty Property,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		UE::UAI::Contract::FDiagnostic NativeError;
		if (!UeAgentMaterialAuthoring::ConnectExpressionToProperty(
				Expression, OutputName, Property, NativeError))
		{
			OutError = NativeError;
			return false;
		}
		UObject* Owner = Expression.GetOuter();
		if (!Owner)
		{
			OutError = NativeConditionFailure(TEXT("material_niagara_graph_owner_type_invalid"),
				TEXT("The connected material expression has no owning object."), TEXT("/expression/owner_present"), &Expression);
			OutError.MutationMayHaveStarted = true;
			return false;
		}
		UeAgentMaterialAuthoring::RefreshGraphOwner(*Owner);
		SetMutationReceipt(*Owner, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TryConnectMaterialExpressionToPropertyByIndex(
		UMaterialExpression& Expression,
		const int32 OutputIndex,
		const EMaterialProperty Property,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		UMaterial* const Material = Cast<UMaterial>(Expression.GetOuter());
		FExpressionInput* const Input = Material
			? Material->GetExpressionInputForProperty(Property)
			: nullptr;
		if (!Material)
		{
			OutError = NativeConditionFailure(TEXT("material_niagara_graph_root_connection_failed"),
				TEXT("The expression is not owned by a material root."), TEXT("/material_root_present"), &Expression);
			return false;
		}
		if (!Input)
		{
			const auto Requested = MakeShared<FJsonObject>();
			Requested->SetStringField(TEXT("type"), TEXT("EMaterialProperty"));
			Requested->SetNumberField(TEXT("value"), static_cast<int32>(Property));
			OutError = NativeValueFailure(TEXT("material_niagara_graph_root_connection_failed"),
				TEXT("The material root has no input for the requested material property."), TEXT("/material_property"),
				MakeShared<FJsonValueObject>(Requested), MakeShared<FJsonValueString>(TEXT("material property with a root input")), &Expression);
			return false;
		}
		if (!Expression.GetOutputs().IsValidIndex(OutputIndex))
		{
			OutError = NativeValueFailure(TEXT("material_niagara_graph_root_connection_failed"),
				TEXT("The requested material expression output index does not exist."), TEXT("/output_index"),
				MakeShared<FJsonValueNumber>(OutputIndex),
				MakeShared<FJsonValueString>(FString::Printf(TEXT("integer in [0, %d)"), Expression.GetOutputs().Num())), &Expression);
			return false;
		}
		Material->Modify();
		Expression.ConnectExpression(Input, OutputIndex);
		UeAgentMaterialAuthoring::RefreshGraphOwner(*Material);
		SetMutationReceipt(*Material, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TryConnectMaterialExpressionsByIndex(
		UMaterialExpression& FromExpression,
		const int32 FromOutputIndex,
		UMaterialExpression& ToExpression,
		const FString& ToInputName,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		FExpressionInput* Input = nullptr;
		if (FromExpression.GetOuter() != ToExpression.GetOuter())
		{
			OutError = NativeConditionFailure(TEXT("material_niagara_graph_expression_connection_failed"),
				TEXT("The source and destination expressions have different native owners."), TEXT("/expression_owners_match"), &FromExpression);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(GetPathNameSafe(ToExpression.GetOuter()));
			return false;
		}
		if (!FromExpression.GetOutputs().IsValidIndex(FromOutputIndex))
		{
			OutError = NativeValueFailure(TEXT("material_niagara_graph_expression_connection_failed"),
				TEXT("The requested source expression output index does not exist."), TEXT("/source_output_index"),
				MakeShared<FJsonValueNumber>(FromOutputIndex),
				MakeShared<FJsonValueString>(FString::Printf(TEXT("integer in [0, %d)"), FromExpression.GetOutputs().Num())), &FromExpression);
			return false;
		}
		if (!UeAgentMaterialAuthoring::ResolveExpressionInput(ToExpression, ToInputName, Input) || !Input)
		{
			OutError = NativeValueFailure(TEXT("material_niagara_graph_expression_connection_failed"),
				TEXT("The destination expression input could not be resolved."), TEXT("/destination_input_name"),
				MakeShared<FJsonValueString>(ToInputName), MakeShared<FJsonValueString>(TEXT("resolvable expression input name")), &ToExpression);
			return false;
		}
		ToExpression.Modify();
		FromExpression.ConnectExpression(Input, FromOutputIndex);
		UObject* const Owner = FromExpression.GetOuter();
		if (!Owner)
		{
			OutError = NativeConditionFailure(TEXT("material_niagara_graph_owner_type_invalid"),
				TEXT("The connected expressions have no owning material object."), TEXT("/expression/owner_present"), &FromExpression);
			OutError.MutationStarted = true;
			return false;
		}
		UeAgentMaterialAuthoring::RefreshGraphOwner(*Owner);
		SetMutationReceipt(*Owner, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TrySetMaterialExpressionLinearColorProperty(
		UMaterialExpression& Expression,
		const FString& PropertyPath,
		const FLinearColor& Value,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		FProperty* const Found = FindFProperty<FProperty>(Expression.GetClass(), *PropertyPath);
		FStructProperty* const Property = CastField<FStructProperty>(Found);
		if (!Property || Property->Struct != TBaseStructure<FLinearColor>::Get()
			|| !Property->HasAnyPropertyFlags(CPF_Edit))
		{
			if (!Property)
				OutError = NativeValueFailure(TEXT("material_niagara_graph_expression_property_invalid"),
					Found ? TEXT("The material expression field is not a struct property.") : TEXT("The material expression property does not exist."),
					PropertyPath, Found ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Found->GetCPPType())) : TSharedPtr<FJsonValue>(),
					MakeShared<FJsonValueString>(TEXT("editable FLinearColor property")), &Expression);
			else if (Property->Struct != TBaseStructure<FLinearColor>::Get())
				OutError = NativeValueFailure(TEXT("material_niagara_graph_expression_property_invalid"),
					TEXT("The material expression struct property is not FLinearColor."), PropertyPath,
					Property->Struct ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Property->Struct->GetPathName())) : TSharedPtr<FJsonValue>(),
					MakeShared<FJsonValueString>(TBaseStructure<FLinearColor>::Get()->GetPathName()), &Expression);
			else OutError = NativeConditionFailure(TEXT("material_niagara_graph_expression_property_invalid"),
				TEXT("The material expression color property is not editable."), PropertyPath + TEXT("/editable"), &Expression);
			return false;
		}
		Expression.Modify();
		*Property->ContainerPtrToValuePtr<FLinearColor>(&Expression) = Value;
		FPropertyChangedEvent Changed(Property);
		Expression.PostEditChangeProperty(Changed);
		UObject* const Owner = Expression.GetOuter();
		if (!Owner)
		{
			OutError = NativeConditionFailure(TEXT("material_niagara_graph_owner_type_invalid"),
				TEXT("The modified material expression has no owning object."), TEXT("/expression/owner_present"), &Expression);
			OutError.MutationStarted = true;
			return false;
		}
		UeAgentMaterialAuthoring::RefreshGraphOwner(*Owner);
		SetMutationReceipt(*Owner, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TrySetMaterialExpressionNameProperty(
		UMaterialExpression& Expression,
		const FString& PropertyPath,
		const FName& Value,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		FProperty* const Found = FindFProperty<FProperty>(Expression.GetClass(), *PropertyPath);
		FNameProperty* const Property = CastField<FNameProperty>(Found);
		if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
		{
			OutError = !Property ? NativeValueFailure(TEXT("material_niagara_graph_expression_property_invalid"),
				Found ? TEXT("The material expression field is not an FName property.") : TEXT("The material expression property does not exist."),
				PropertyPath, Found ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Found->GetCPPType())) : TSharedPtr<FJsonValue>(),
				MakeShared<FJsonValueString>(TEXT("editable FName property")), &Expression)
				: NativeConditionFailure(TEXT("material_niagara_graph_expression_property_invalid"),
					TEXT("The material expression name property is not editable."), PropertyPath + TEXT("/editable"), &Expression);
			return false;
		}
		Expression.Modify();
		Property->SetPropertyValue_InContainer(&Expression, Value);
		FPropertyChangedEvent Changed(Property);
		Expression.PostEditChangeProperty(Changed);
		UObject* const Owner = Expression.GetOuter();
		if (!Owner)
		{
			OutError = NativeConditionFailure(TEXT("material_niagara_graph_owner_type_invalid"),
				TEXT("The modified material expression has no owning object."), TEXT("/expression/owner_present"), &Expression);
			OutError.MutationStarted = true;
			return false;
		}
		UeAgentMaterialAuthoring::RefreshGraphOwner(*Owner);
		SetMutationReceipt(*Owner, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::TryCaptureMaterialGraph(
		UObject& Owner,
		const FString& TargetIdentity,
		FMaterialNiagaraGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutTopology = FMaterialNiagaraGraphTopology();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		OutError.Reset();
		const FMaterialExpressionCollection* Collection =
			UeAgentMaterialAuthoring::GetExpressionCollection(Owner);
		if (!Collection || TargetIdentity.IsEmpty())
		{
			OutError = !Collection ? NativeValueFailure(TEXT("material_niagara_graph_capture_target_invalid"),
				TEXT("The native object has no material expression collection to capture."), TEXT("/owner_class"),
				MakeShared<FJsonValueString>(Owner.GetClass()->GetPathName()),
				MakeShared<FJsonValueString>(TEXT("material or material function graph owner")), &Owner)
				: NativeConditionFailure(TEXT("material_niagara_graph_capture_target_invalid"),
					TEXT("The material graph capture has no target identity."), TEXT("/target_identity_present"), &Owner);
			return false;
		}

		TArray<FMaterialNiagaraGraphItemIdentity> Items;
		TArray<FMaterialNiagaraGraphPinIdentity> Pins;
		TArray<FMaterialNiagaraGraphLinkIdentity> Links;
		Items.Emplace(TEXT("material:root"), FString(), TEXT("material_root"));
		TMap<const UMaterialExpression*, FString> ItemKeys;
		for (UMaterialExpression* Expression : Collection->Expressions)
		{
			if (!Expression || !Expression->MaterialExpressionGuid.IsValid())
			{
				OutError = NativeConditionFailure(TEXT("material_niagara_graph_item_identity_missing"),
					!Expression ? TEXT("The native material expression collection contains an unavailable expression.")
						: TEXT("A native material expression has no valid identity."),
					TEXT("/expressions/") + FString::FromInt(Items.Num() - 1) + (!Expression ? TEXT("/object_present") : TEXT("/native_identity_valid")), &Owner);
				if (Expression)
				{
					OutError.CandidateName = Expression->GetClass()->GetName();
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Expression->GetPathName());
				}
				return false;
			}
			const FString ItemKey = ExpressionItemKey(*Expression);
			ItemKeys.Add(Expression, ItemKey);
			Items.Emplace(
				ItemKey,
				GuidText(Expression->MaterialExpressionGuid),
				FString());
			const TArray<FExpressionOutput>& Outputs =
				Expression->GetOutputs();
			for (int32 Index = 0; Index < Outputs.Num(); ++Index)
			{
				Pins.Emplace(
					OutputPinKey(*Expression, Index),
					ItemKey,
					Outputs[Index].OutputName.ToString(),
					Index,
					EMaterialNiagaraGraphPinDirection::Output);
			}
		}

		for (UMaterialExpression* Expression : Collection->Expressions)
		{
			if (!Expression)
			{
				continue;
			}
			for (FExpressionInputIterator It{Expression}; It; ++It)
			{
				const FString InputName = MaterialInputName(
					*Expression, It.Index, *It.Input);
				const FString ToPinKey = InputPinKey(
					*Expression, It.Index, InputName);
				Pins.Emplace(
					ToPinKey,
					ExpressionItemKey(*Expression),
					InputName,
					It.Index,
					EMaterialNiagaraGraphPinDirection::Input);
				if (It->Expression && ItemKeys.Contains(It->Expression))
				{
					const FString FromPinKey = OutputPinKey(
						*It->Expression, It->OutputIndex);
					Links.Emplace(
						TEXT("link:") + FromPinKey + TEXT("->") + ToPinKey,
						FromPinKey,
						ToPinKey);
				}
			}
		}

		if (UMaterial* Material = Cast<UMaterial>(&Owner))
		{
			for (const EMaterialProperty Property
				: UeAgentMaterialAuthoring::GetSupportedMaterialProperties())
			{
				const FString ToPinKey = RootPinKey(Property);
				Pins.Emplace(
					ToPinKey,
					TEXT("material:root"),
					MaterialPropertyToken(Property),
					INDEX_NONE,
					EMaterialNiagaraGraphPinDirection::Input);
				FExpressionInput* Input =
					Material->GetExpressionInputForProperty(Property);
				if (Input && Input->Expression
					&& ItemKeys.Contains(Input->Expression))
				{
					const FString FromPinKey = OutputPinKey(
						*Input->Expression, Input->OutputIndex);
					Links.Emplace(
						TEXT("link:") + FromPinKey + TEXT("->") + ToPinKey,
						FromPinKey,
						ToPinKey);
				}
			}
		}

		FMaterialNiagaraGraphTopology Captured(
			Cast<UMaterial>(&Owner) ? TEXT("material")
				: TEXT("material_function"),
			TargetIdentity,
			TEXT("material_graph:root"),
			MoveTemp(Items),
			MoveTemp(Pins),
			MoveTemp(Links),
			{});
		if (!TryValidateMaterialNiagaraGraphTopology(Captured, OutError))
		{
			return false;
		}
		OutRevision = TopologyRevision(Captured);
		OutEvidenceId = NewEvidence(TEXT("readback"), &Owner);
		OutTopology = MoveTemp(Captured);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TrySetMaterialInstanceScalarParameter(
		UMaterialInstanceConstant& MaterialInstance,
		const FMaterialParameterInfo& ParameterInfo,
		const float Value,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		const TArray<FMaterialNiagaraGraphParameterIdentity> Identity = {
			ParameterIdentity(
				ParameterInfo,
				EMaterialNiagaraGraphParameterType::Scalar),
		};
		if (!TryValidateMaterialNiagaraGraphParameterIdentities(
				Identity, OutError))
		{
			return false;
		}
		MaterialInstance.Modify();
		MaterialInstance.SetScalarParameterValueEditorOnly(
			ParameterInfo, Value);
		UMaterialEditingLibrary::UpdateMaterialInstance(&MaterialInstance);
		MaterialInstance.MarkPackageDirty();
		SetMutationReceipt(MaterialInstance, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TrySetMaterialInstanceVectorParameter(
		UMaterialInstanceConstant& MaterialInstance,
		const FMaterialParameterInfo& ParameterInfo,
		const FLinearColor& Value,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		const TArray<FMaterialNiagaraGraphParameterIdentity> Identity = {
			ParameterIdentity(
				ParameterInfo,
				EMaterialNiagaraGraphParameterType::Vector),
		};
		if (!TryValidateMaterialNiagaraGraphParameterIdentities(
				Identity, OutError))
		{
			return false;
		}
		MaterialInstance.Modify();
		MaterialInstance.SetVectorParameterValueEditorOnly(
			ParameterInfo, Value);
		UMaterialEditingLibrary::UpdateMaterialInstance(&MaterialInstance);
		MaterialInstance.MarkPackageDirty();
		SetMutationReceipt(MaterialInstance, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TrySetMaterialInstanceTextureParameter(
		UMaterialInstanceConstant& MaterialInstance,
		const FMaterialParameterInfo& ParameterInfo,
		UTexture& Value,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		const TArray<FMaterialNiagaraGraphParameterIdentity> Identity = {
			ParameterIdentity(
				ParameterInfo,
				EMaterialNiagaraGraphParameterType::Texture),
		};
		if (!TryValidateMaterialNiagaraGraphParameterIdentities(
				Identity, OutError))
		{
			return false;
		}
		MaterialInstance.Modify();
		MaterialInstance.SetTextureParameterValueEditorOnly(
			ParameterInfo, &Value);
		UMaterialEditingLibrary::UpdateMaterialInstance(&MaterialInstance);
		MaterialInstance.MarkPackageDirty();
		SetMutationReceipt(MaterialInstance, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TrySetMaterialInstanceStaticSwitchParameter(
		UMaterialInstanceConstant& MaterialInstance,
		const FMaterialParameterInfo& ParameterInfo,
		const bool bValue,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		const TArray<FMaterialNiagaraGraphParameterIdentity> Identity = {
			ParameterIdentity(
				ParameterInfo,
				EMaterialNiagaraGraphParameterType::StaticSwitch),
		};
		if (!TryValidateMaterialNiagaraGraphParameterIdentities(
				Identity, OutError))
		{
			return false;
		}
		MaterialInstance.Modify();
		MaterialInstance.SetStaticSwitchParameterValueEditorOnly(
			ParameterInfo, bValue);
		UMaterialEditingLibrary::UpdateMaterialInstance(&MaterialInstance);
		MaterialInstance.MarkPackageDirty();
		SetMutationReceipt(MaterialInstance, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TryCaptureMaterialInstanceEffectiveParameters(
		UMaterialInstanceConstant& MaterialInstance,
		TArray<FMaterialNiagaraGraphEffectiveParameter>& OutParameters,
		FString& OutRevision,
		FString& OutEvidenceId,
		FMaterialNiagaraGraphAdapterError& OutError,
		const bool bAllowNullTextures) const
	{
		OutParameters.Reset();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		OutError.Reset();
		TArray<FString> RevisionFacts;
		auto IsScalarOverride = [&MaterialInstance](
			const FMaterialParameterInfo& Info)
		{
			return MaterialInstance.ScalarParameterValues.ContainsByPredicate(
				[&](const FScalarParameterValue& Value)
				{
					return Value.ParameterInfo == Info;
				});
		};
		auto IsVectorOverride = [&MaterialInstance](
			const FMaterialParameterInfo& Info)
		{
			return MaterialInstance.VectorParameterValues.ContainsByPredicate(
				[&](const FVectorParameterValue& Value)
				{
					return Value.ParameterInfo == Info;
				});
		};
		auto IsTextureOverride = [&MaterialInstance](
			const FMaterialParameterInfo& Info)
		{
			return MaterialInstance.TextureParameterValues.ContainsByPredicate(
				[&](const FTextureParameterValue& Value)
				{
					return Value.ParameterInfo == Info;
				});
		};
		const FStaticParameterSet StaticParameters =
			MaterialInstance.GetStaticParameters();
		auto IsSwitchOverride = [&StaticParameters](
			const FMaterialParameterInfo& Info)
		{
			return StaticParameters.StaticSwitchParameters.ContainsByPredicate(
				[&](const FStaticSwitchParameter& Value)
				{
					return Value.ParameterInfo == Info && Value.bOverride;
				});
		};
		auto Add = [&OutParameters, &RevisionFacts](
			FMaterialNiagaraGraphParameterIdentity Identity,
			const bool bOverride,
			const float Scalar,
			const FLinearColor& Vector,
			FString TextureRef,
			const bool bBoolean,
			FString ValueFact)
		{
			RevisionFacts.Add(
				Identity.GetParameterKey() + TEXT("|") + MoveTemp(ValueFact)
				+ (bOverride ? TEXT("|override") : TEXT("|inherited")));
			OutParameters.Emplace(
				MoveTemp(Identity), bOverride, Scalar, Vector,
				MoveTemp(TextureRef), bBoolean);
		};

		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		MaterialInstance.GetAllScalarParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			float Value = 0.0f;
			if (!MaterialInstance.GetScalarParameterValue(
					FHashedMaterialParameterInfo(Info), Value))
			{
				OutError = ParameterReadFailure(ParameterIdentity(Info, EMaterialNiagaraGraphParameterType::Scalar),
					TEXT("scalar"), TEXT("Unreal could not read the effective material scalar parameter."), &MaterialInstance);
				return false;
			}
			Add(ParameterIdentity(Info,
				EMaterialNiagaraGraphParameterType::Scalar),
				IsScalarOverride(Info), Value, FLinearColor::Black,
				FString(), false, FString::SanitizeFloat(Value));
		}
		Infos.Reset();
		Ids.Reset();
		MaterialInstance.GetAllVectorParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			FLinearColor Value = FLinearColor::Black;
			if (!MaterialInstance.GetVectorParameterValue(
					FHashedMaterialParameterInfo(Info), Value))
			{
				OutError = ParameterReadFailure(ParameterIdentity(Info, EMaterialNiagaraGraphParameterType::Vector),
					TEXT("vector"), TEXT("Unreal could not read the effective material vector parameter."), &MaterialInstance);
				return false;
			}
			Add(ParameterIdentity(Info,
				EMaterialNiagaraGraphParameterType::Vector),
				IsVectorOverride(Info), 0.0f, Value,
				FString(), false, Value.ToString());
		}
		Infos.Reset();
		Ids.Reset();
		MaterialInstance.GetAllTextureParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			UTexture* Value = nullptr;
			const bool bReadTexture = MaterialInstance.GetTextureParameterValue(FHashedMaterialParameterInfo(Info), Value);
			if (!bReadTexture || (!Value && !bAllowNullTextures))
			{
				OutError = ParameterReadFailure(ParameterIdentity(Info, EMaterialNiagaraGraphParameterType::Texture), TEXT("texture"),
					bReadTexture ? TEXT("The effective material texture is null while null textures are disabled.")
						: TEXT("Unreal could not read the effective material texture parameter."),
					&MaterialInstance, bReadTexture ? TEXT("texture_present") : TEXT("value_read_succeeded"));
				return false;
			}
			const FString Ref = Value ? Value->GetPathName() : FString();
			Add(ParameterIdentity(Info,
				EMaterialNiagaraGraphParameterType::Texture),
				IsTextureOverride(Info), 0.0f, FLinearColor::Black,
				Ref, false, Ref);
		}
		Infos.Reset();
		Ids.Reset();
		MaterialInstance.GetAllStaticSwitchParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			bool bValue = false;
			FGuid ExpressionGuid;
			if (!MaterialInstance.GetStaticSwitchParameterValue(
					FHashedMaterialParameterInfo(Info), bValue,
					ExpressionGuid))
			{
				OutError = ParameterReadFailure(ParameterIdentity(Info, EMaterialNiagaraGraphParameterType::StaticSwitch),
					TEXT("static_switch"), TEXT("Unreal could not read the effective material static switch parameter."), &MaterialInstance);
				return false;
			}
			Add(ParameterIdentity(Info,
				EMaterialNiagaraGraphParameterType::StaticSwitch),
				IsSwitchOverride(Info), 0.0f, FLinearColor::Black,
				FString(), bValue, bValue ? TEXT("true") : TEXT("false"));
		}
		TArray<FMaterialNiagaraGraphParameterIdentity> Identities;
		for (int32 ParameterIndex = 0; ParameterIndex < OutParameters.Num(); ++ParameterIndex)
		{
			const auto& Parameter = OutParameters[ParameterIndex];
			if (!Parameter.IsValid())
			{
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("material_niagara_graph_parameter_capture_failed"),
					*Parameter.GetValidationDiagnostic());
				OutError.PropertyPath = TEXT("/effective_parameters/") + FString::FromInt(ParameterIndex) + OutError.PropertyPath;
				OutParameters.Reset();
				return false;
			}
			Identities.Add(Parameter.GetIdentity());
		}
		if (!TryValidateMaterialNiagaraGraphParameterIdentities(
				Identities, OutError))
		{
			OutParameters.Reset();
			return false;
		}
		OutRevision = ParameterRevision(MoveTemp(RevisionFacts));
		OutEvidenceId = NewEvidence(TEXT("readback"), &MaterialInstance);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TryCaptureMaterialInstanceParameters(
		UMaterialInstanceConstant& MaterialInstance,
		TArray<FMaterialNiagaraGraphParameterIdentity>& OutParameters,
		FString& OutRevision,
		FString& OutEvidenceId,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutParameters.Reset();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		OutError.Reset();
		TArray<FString> RevisionFacts;
		auto AddParameter = [&OutParameters, &RevisionFacts](
			const FMaterialParameterInfo& Info,
			const EMaterialNiagaraGraphParameterType Type,
			FString ValueFact)
		{
			FMaterialNiagaraGraphParameterIdentity Identity =
				ParameterIdentity(Info, Type);
			RevisionFacts.Add(
				Identity.GetParameterKey() + TEXT("|") + MoveTemp(ValueFact));
			OutParameters.Add(MoveTemp(Identity));
		};

		for (const FScalarParameterValue& Parameter
			: MaterialInstance.ScalarParameterValues)
		{
			AddParameter(
				Parameter.ParameterInfo,
				EMaterialNiagaraGraphParameterType::Scalar,
				FString::SanitizeFloat(Parameter.ParameterValue));
		}
		for (const FVectorParameterValue& Parameter
			: MaterialInstance.VectorParameterValues)
		{
			AddParameter(
				Parameter.ParameterInfo,
				EMaterialNiagaraGraphParameterType::Vector,
				Parameter.ParameterValue.ToString());
		}
		for (const FTextureParameterValue& Parameter
			: MaterialInstance.TextureParameterValues)
		{
			AddParameter(
				Parameter.ParameterInfo,
				EMaterialNiagaraGraphParameterType::Texture,
				Parameter.ParameterValue
					? Parameter.ParameterValue->GetPathName()
					: TEXT("null"));
		}
		const FStaticParameterSet StaticParameters =
			MaterialInstance.GetStaticParameters();
		for (const FStaticSwitchParameter& Parameter
			: StaticParameters.StaticSwitchParameters)
		{
			if (Parameter.bOverride)
			{
				AddParameter(
					Parameter.ParameterInfo,
					EMaterialNiagaraGraphParameterType::StaticSwitch,
					Parameter.Value ? TEXT("true") : TEXT("false"));
			}
		}
		for (const FStaticComponentMaskParameter& Parameter
			: StaticParameters.EditorOnly.StaticComponentMaskParameters)
		{
			if (Parameter.bOverride)
			{
				AddParameter(
					Parameter.ParameterInfo,
					EMaterialNiagaraGraphParameterType::StaticComponentMask,
					FString::Printf(
						TEXT("%d%d%d%d"),
						Parameter.R ? 1 : 0,
						Parameter.G ? 1 : 0,
						Parameter.B ? 1 : 0,
						Parameter.A ? 1 : 0));
			}
		}
		if (!TryValidateMaterialNiagaraGraphParameterIdentities(
				OutParameters, OutError))
		{
			OutParameters.Reset();
			return false;
		}
		OutRevision = ParameterRevision(MoveTemp(RevisionFacts));
		OutEvidenceId = NewEvidence(TEXT("readback"), &MaterialInstance);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::TryCompileAndSettleMaterial(
		UObject& Owner,
		FMaterialNiagaraGraphCompileSettleReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt = FMaterialNiagaraGraphCompileSettleReceipt();
		OutError.Reset();
		TArray<FMaterialNiagaraGraphAdapterError> CompileErrors;
		if (UMaterial* Material = Cast<UMaterial>(&Owner))
		{
			UMaterialEditingLibrary::RecompileMaterial(Material);
			OutReceipt.CompileErrorCount =
				SettleMaterialCompileErrors(*Material, CompileErrors);
		}
		else if (UMaterialInstanceConstant* MaterialInstance =
			Cast<UMaterialInstanceConstant>(&Owner))
		{
			UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
			OutReceipt.CompileErrorCount =
				SettleMaterialCompileErrors(*MaterialInstance, CompileErrors);
		}
		else if (UMaterialFunction* Function = Cast<UMaterialFunction>(&Owner))
		{
			// A function owns a definition update, not an independent shader resource.
			// Existing preview resources are reported separately by diagnostics.
			UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
		}
		else
		{
			OutError = NativeValueFailure(TEXT("material_niagara_graph_compile_target_invalid"),
				TEXT("This native object cannot be compiled as a material or material function."), TEXT("/owner_class"),
				MakeShared<FJsonValueString>(Owner.GetClass()->GetPathName()),
				MakeShared<FJsonValueString>(TEXT("Material, MaterialInstanceConstant or MaterialFunction")), &Owner);
			return false;
		}
		OutReceipt.bCompilationComplete = true;
		OutReceipt.CompileEvidenceId = NewEvidence(TEXT("compile"), &Owner);
		OutReceipt.SettleEvidenceId = NewEvidence(TEXT("settle"), &Owner);
		if (OutReceipt.CompileErrorCount != 0)
		{
			OutError = NativeValueFailure(TEXT("material_niagara_graph_compile_incomplete"),
				TEXT("The native material compiler reported errors."), TEXT("/compile_error_count"),
				MakeShared<FJsonValueNumber>(OutReceipt.CompileErrorCount), MakeShared<FJsonValueNumber>(0), &Owner);
			OutError.Diagnostics = MoveTemp(CompileErrors);
			return false;
		}
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::
	TryAddNiagaraAssignmentModule(
		UObject& Asset,
		const FGuid& EmitterHandleId,
		const ENiagaraScriptUsage Usage,
		const FGuid& UsageId,
		const TArray<FNiagaraVariable>& AssignmentTargets,
		const int32 TargetIndex,
		UNiagaraNodeFunctionCall*& OutModule,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutModule = nullptr;
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		FResolvedStage Stage;
		if (!TryResolveStage(
				Asset, EmitterHandleId, Usage, UsageId, Stage, OutError))
		{
			return false;
		}
		Asset.Modify();
		Stage.ScriptSource->Modify();
		Stage.Graph->Modify();
		Stage.OutputNode->Modify();
		UE::UAI::Contract::FDiagnostic NativeError;
		OutModule = AddAssignmentModule(
			*Stage.OutputNode, AssignmentTargets, TargetIndex, NativeError);
		if (!OutModule)
		{
			OutError = NativeError;
			return false;
		}
		if (!OutModule->NodeGuid.IsValid())
		{
			OutModule->CreateNewGuid();
		}
		Stage.Graph->NotifyGraphChanged();
		Asset.MarkPackageDirty();
		SetMutationReceipt(Asset, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::TryMoveNiagaraModule(
		UObject& Asset,
		const FGuid& EmitterHandleId,
		const ENiagaraScriptUsage Usage,
		const FGuid& UsageId,
		UNiagaraNodeFunctionCall& Module,
		const int32 TargetIndex,
		int32& OutNewIndex,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutNewIndex = INDEX_NONE;
		OutReceipt.SetEvidenceId(FString());
		OutError.Reset();
		FResolvedStage Stage;
		if (!TryResolveStage(
				Asset, EmitterHandleId, Usage, UsageId, Stage, OutError))
		{
			return false;
		}
		Asset.Modify();
		Stage.ScriptSource->Modify();
		Stage.Graph->Modify();
		Stage.OutputNode->Modify();
		UE::UAI::Contract::FDiagnostic NativeError;
		if (!MoveModule(
				*Stage.OutputNode,
				Module,
				TargetIndex,
				OutNewIndex,
				NativeError))
		{
			OutError = NativeError;
			return false;
		}
		Stage.Graph->NotifyGraphChanged();
		Asset.MarkPackageDirty();
		SetMutationReceipt(Asset, OutReceipt);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::TryCaptureNiagaraStack(
		UObject& Asset,
		const FGuid& EmitterHandleId,
		const ENiagaraScriptUsage Usage,
		const FGuid& UsageId,
		FMaterialNiagaraGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutTopology = FMaterialNiagaraGraphTopology();
		OutRevision.Reset();
		OutEvidenceId.Reset();
		OutError.Reset();
		FResolvedStage Stage;
		if (!TryResolveStage(
				Asset, EmitterHandleId, Usage, UsageId, Stage, OutError))
		{
			return false;
		}
		TArray<UNiagaraNodeFunctionCall*> Modules;
		if (!GetModules(*Stage.OutputNode, Modules, OutError)) return false;
		TArray<FMaterialNiagaraGraphItemIdentity> Items;
		TArray<FMaterialNiagaraGraphOrderingIdentity> Ordering;
		FString Predecessor;
		for (int32 Index = 0; Index < Modules.Num(); ++Index)
		{
			UNiagaraNodeFunctionCall* Module = Modules[Index];
			if (!Module || !Module->NodeGuid.IsValid())
			{
				OutError = NativeConditionFailure(TEXT("material_niagara_graph_item_identity_missing"),
					!Module ? TEXT("The native Niagara stack contains an unavailable module.") : TEXT("A native Niagara stack module has no valid identity."),
					TEXT("/modules/") + FString::FromInt(Index) + (!Module ? TEXT("/object_present") : TEXT("/native_identity_valid")), &Asset);
				if (Module) OutError.ActualEvidence = MakeShared<FJsonValueString>(Module->GetPathName());
				return false;
			}
			const FString ItemKey = ModuleItemKey(*Module);
			Items.Emplace(ItemKey, GuidText(Module->NodeGuid), FString());
			Ordering.Emplace(
				FString::Printf(TEXT("order:%s:%d"), *Stage.StageKey, Index),
				ItemKey,
				Predecessor);
			Predecessor = ItemKey;
		}
		FMaterialNiagaraGraphTopology Captured(
			TEXT("niagara_stack"),
			Asset.GetPathName(),
			Stage.StageKey,
			MoveTemp(Items), {}, {}, MoveTemp(Ordering));
		if (!TryValidateMaterialNiagaraGraphTopology(Captured, OutError))
		{
			OutTopology = FMaterialNiagaraGraphTopology();
			return false;
		}
		OutRevision = TopologyRevision(Captured);
		OutEvidenceId = NewEvidence(TEXT("readback"), &Asset);
		OutTopology = MoveTemp(Captured);
		return true;
	}

	bool FMaterialNiagaraGraphNativeHook::TrySetNiagaraSystemWarmupTime(
		UNiagaraSystem& System,
		const float WarmupTime,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt = FMaterialNiagaraGraphNativeMutationReceipt();
		OutError.Reset();
		if (!FMath::IsFinite(WarmupTime) || WarmupTime < 0.0f)
		{
			OutError = NativeValueFailure(TEXT("material_niagara_graph_warmup_time_invalid"),
				TEXT("Niagara warmup time must be finite and nonnegative."), TEXT("/warmup_time"),
				FiniteNumberFact(WarmupTime), MakeShared<FJsonValueString>(TEXT("finite number >= 0")), &System);
			return false;
		}
		System.Modify();
		System.SetWarmupTime(WarmupTime);
		System.PostEditChange();
		System.MarkPackageDirty();
		SetMutationReceipt(System, OutReceipt);
		return !OutReceipt.GetEvidenceId().IsEmpty();
	}

	bool FMaterialNiagaraGraphNativeHook::TrySetNiagaraEmitterLocalSpace(
		UNiagaraEmitter& Emitter,
		const bool bLocalSpace,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt = FMaterialNiagaraGraphNativeMutationReceipt();
		OutError.Reset();
		FVersionedNiagaraEmitterData* Data = Emitter.GetLatestEmitterData();
		if (!Data)
		{
			OutError = NativeConditionFailure(TEXT("material_niagara_graph_emitter_data_missing"),
				TEXT("The Niagara emitter has no latest version data to update."), TEXT("/latest_emitter_data_present"), &Emitter);
			return false;
		}
		Emitter.Modify();
		Data->bLocalSpace = bLocalSpace;
		Emitter.PostEditChange();
		Emitter.MarkPackageDirty();
		SetMutationReceipt(Emitter, OutReceipt);
		return !OutReceipt.GetEvidenceId().IsEmpty();
	}

	bool FMaterialNiagaraGraphNativeHook::TrySetNiagaraScriptDescription(
		UNiagaraScript& Script,
		const FText& Description,
		FMaterialNiagaraGraphNativeMutationReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt = FMaterialNiagaraGraphNativeMutationReceipt();
		OutError.Reset();
		FVersionedNiagaraScriptData* Data = Script.GetLatestScriptData();
		if (!Data)
		{
			OutError = NativeConditionFailure(TEXT("material_niagara_graph_script_data_missing"),
				TEXT("The Niagara script has no latest version data to update."), TEXT("/latest_script_data_present"), &Script);
			return false;
		}
		Script.Modify();
		Data->Description = Description;
		Script.PostEditChange();
		Script.MarkPackageDirty();
		SetMutationReceipt(Script, OutReceipt);
		return !OutReceipt.GetEvidenceId().IsEmpty();
	}

	bool FMaterialNiagaraGraphNativeHook::TryCompileAndSettleNiagara(
		UObject& Asset,
		FMaterialNiagaraGraphCompileSettleReceipt& OutReceipt,
		FMaterialNiagaraGraphAdapterError& OutError) const
	{
		OutReceipt = FMaterialNiagaraGraphCompileSettleReceipt();
		OutError.Reset();
		UeAgentNiagaraCompileValidation::FResult Compile;
		if (UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset))
		{
			System->RequestCompile(true);
			System->WaitForCompilationComplete(true, false);
			System->PollForCompilationComplete(true);
			if (System->HasOutstandingCompilationRequests(true))
			{
				OutError = NativeConditionFailure(TEXT("material_niagara_graph_compile_incomplete"),
					TEXT("Niagara still has outstanding compilation requests after waiting and polling."),
					TEXT("/outstanding_compilation_requests"), &Asset, true, false);
				return false;
			}
			System->CacheFromCompiledData();
			Compile = UeAgentNiagaraCompileValidation::InspectAsset(System);
		}
		else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset))
		{
			UeAgentNiagaraEmitterCompileContext::FResult Result;
			UE::UAI::Contract::FDiagnostic NativeError;
			if (!UeAgentNiagaraEmitterCompileContext::Compile(
					*Emitter,
					Emitter->GetExposedVersion().VersionGuid,
					true,
					Result,
					NativeError))
			{
				OutError = NativeError;
				return false;
			}
			Compile = Result.Validation;
		}
		else if (UNiagaraScript* Script = Cast<UNiagaraScript>(&Asset))
		{
			Script->RequestCompile(
				Script->GetExposedVersion().VersionGuid,
				true);
			Compile = UeAgentNiagaraCompileValidation::InspectAsset(Script);
		}
		else
		{
			OutError = NativeValueFailure(TEXT("material_niagara_graph_compile_target_invalid"),
				TEXT("This native object cannot be compiled as a Niagara asset."), TEXT("/owner_class"),
				MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()),
				MakeShared<FJsonValueString>(TEXT("NiagaraSystem, NiagaraEmitter or NiagaraScript")), &Asset);
			return false;
		}

		const UeAgentNiagaraStackValidation::FResult Stack =
			UeAgentNiagaraStackValidation::InspectAsset(&Asset);
		OutReceipt.bCompilationComplete = Compile.bCompilationComplete;
		OutReceipt.CompileErrorCount = Compile.ErrorScriptCount;
		OutReceipt.StackIssueCount = Stack.GetBlockingIssueCount();
		OutReceipt.CompileEvidenceId = NewEvidence(TEXT("compile"), &Asset);
		OutReceipt.SettleEvidenceId = NewEvidence(TEXT("settle"), &Asset);
		if (!Stack.bInspectionComplete)
		{
			OutError = Stack.GetFailure();
			return false;
		}
		if (!OutReceipt.bCompilationComplete
			|| OutReceipt.CompileErrorCount != 0)
		{
			OutError = Compile.GetFailure(Asset.GetPathName());
			return false;
		}
		if (OutReceipt.StackIssueCount != 0)
		{
			OutError = Stack.GetFailure(true);
			return false;
		}
		return true;
	}
}
