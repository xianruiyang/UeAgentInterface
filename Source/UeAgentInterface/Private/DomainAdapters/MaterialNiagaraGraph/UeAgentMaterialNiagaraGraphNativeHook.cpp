#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"

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
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph
{
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
		bValid = FMath::IsFinite(ScalarValue)
			&& TryValidateMaterialNiagaraGraphParameterIdentities(
				{Identity}, ValidationError);
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

		void CaptureRootedNiagaraCompilationCopies(
			TSet<TWeakObjectPtr<UNiagaraScriptSource>>& OutCopies)
		{
			OutCopies.Reset();
			for (TObjectIterator<UNiagaraScriptSource> It; It; ++It)
			{
				UNiagaraScriptSource* Source = *It;
				if (Source
					&& Source->bIsCompilationCopy
					&& !Source->bIsReleased
					&& Source->IsRooted())
				{
					OutCopies.Add(Source);
				}
			}
		}

		void ReleaseNewNiagaraCompilationCopies(
			const TSet<TWeakObjectPtr<UNiagaraScriptSource>>& PreviousCopies)
		{
			for (TObjectIterator<UNiagaraScriptSource> It; It; ++It)
			{
				UNiagaraScriptSource* Source = *It;
				if (!Source
					|| !Source->bIsCompilationCopy
					|| Source->bIsReleased
					|| !Source->IsRooted()
					|| PreviousCopies.Contains(Source))
				{
					continue;
				}

				// UNiagaraScript::RequestCompile is synchronous for standalone
				// scripts in UE 5.6, but unlike the system compilation path it
				// does not release the rooted compilation source after consuming
				// the result. Mirror ReleaseCompilationCopy here for copies owned
				// by this exact request so the authored asset can be unloaded.
				Source->bIsReleased = true;
				Source->NodeGraph = nullptr;
				Source->RemoveFromRoot();
				Source->MarkAsGarbage();
			}
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
			FString NativeError;
			if (!EnumerateStages(Asset, Stages, NativeError))
			{
				OutError.Set(
					TEXT("material_niagara_graph_stage_capture_failed"),
					NativeError);
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
				OutError.Set(
					Matches.IsEmpty()
						? TEXT("material_niagara_graph_stage_not_found")
						: TEXT("material_niagara_graph_stage_ambiguous"),
					FString::Printf(
						TEXT("asset=%s usage=%s usage_id=%s emitter=%s"),
						*Asset.GetPathName(),
						*ScriptUsageToString(Usage),
						*GuidText(UsageId),
						*GuidText(EmitterHandleId)));
				return false;
			}
			OutStage = Matches[0];
			return true;
		}

		int32 CountMaterialCompileErrors(UMaterialInterface& Material)
		{
			int32 ErrorCount = 0;
			UMaterialInterface::IterateOverActiveFeatureLevels(
				[&](const ERHIFeatureLevel::Type FeatureLevel)
				{
					const FMaterialResource* Resource =
						Material.GetMaterialResource(FeatureLevel);
					if (Resource)
					{
						ErrorCount += Resource->GetCompileErrors().Num();
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
			OutError.Set(TEXT("material_niagara_graph_expression_guid_invalid"));
			return false;
		}
		if (!ExpressionClass.IsChildOf(UMaterialExpression::StaticClass())
			|| ExpressionClass.HasAnyClassFlags(CLASS_Abstract))
		{
			OutError.Set(TEXT("material_niagara_graph_expression_class_invalid"));
			return false;
		}
		const FMaterialExpressionCollection* Collection =
			UeAgentMaterialAuthoring::GetExpressionCollection(Owner);
		if (!Collection)
		{
			OutError.Set(TEXT("material_niagara_graph_owner_type_invalid"));
			return false;
		}
		for (UMaterialExpression* Existing : Collection->Expressions)
		{
			if (Existing
				&& Existing->MaterialExpressionGuid == ExpressionGuid)
			{
				OutError.Set(
					TEXT("material_niagara_graph_item_identity_duplicate"),
					GuidText(ExpressionGuid));
				return false;
			}
		}

		Owner.Modify();
		OutExpression = UeAgentMaterialAuthoring::CreateExpression(
			Owner, &ExpressionClass, PositionX, PositionY);
		if (!OutExpression)
		{
			OutError.Set(TEXT("material_niagara_graph_expression_create_failed"));
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
		FString NativeError;
		if (!UeAgentMaterialAuthoring::ConnectExpressionToProperty(
				Expression, OutputName, Property, NativeError))
		{
			OutError.Set(
				TEXT("material_niagara_graph_root_connection_failed"),
				NativeError);
			return false;
		}
		UObject* Owner = Expression.GetOuter();
		if (!Owner)
		{
			OutError.Set(TEXT("material_niagara_graph_owner_type_invalid"));
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
		if (!Material || !Input
			|| !Expression.GetOutputs().IsValidIndex(OutputIndex))
		{
			OutError.Set(
				TEXT("material_niagara_graph_root_connection_failed"),
				TEXT("material root or exact output index is invalid"));
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
		if (FromExpression.GetOuter() != ToExpression.GetOuter()
			|| !FromExpression.GetOutputs().IsValidIndex(FromOutputIndex)
			|| !UeAgentMaterialAuthoring::ResolveExpressionInput(
				ToExpression, ToInputName, Input)
			|| !Input)
		{
			OutError.Set(
				TEXT("material_niagara_graph_expression_connection_failed"),
				TEXT("expression owner, output index or input is invalid"));
			return false;
		}
		ToExpression.Modify();
		FromExpression.ConnectExpression(Input, FromOutputIndex);
		UObject* const Owner = FromExpression.GetOuter();
		if (!Owner)
		{
			OutError.Set(TEXT("material_niagara_graph_owner_type_invalid"));
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
		FStructProperty* const Property =
			FindFProperty<FStructProperty>(Expression.GetClass(), *PropertyPath);
		if (!Property || Property->Struct != TBaseStructure<FLinearColor>::Get()
			|| !Property->HasAnyPropertyFlags(CPF_Edit))
		{
			OutError.Set(
				TEXT("material_niagara_graph_expression_property_invalid"),
				PropertyPath);
			return false;
		}
		Expression.Modify();
		*Property->ContainerPtrToValuePtr<FLinearColor>(&Expression) = Value;
		FPropertyChangedEvent Changed(Property);
		Expression.PostEditChangeProperty(Changed);
		UObject* const Owner = Expression.GetOuter();
		if (!Owner)
		{
			OutError.Set(TEXT("material_niagara_graph_owner_type_invalid"));
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
		FNameProperty* const Property =
			FindFProperty<FNameProperty>(Expression.GetClass(), *PropertyPath);
		if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
		{
			OutError.Set(
				TEXT("material_niagara_graph_expression_property_invalid"),
				PropertyPath);
			return false;
		}
		Expression.Modify();
		Property->SetPropertyValue_InContainer(&Expression, Value);
		FPropertyChangedEvent Changed(Property);
		Expression.PostEditChangeProperty(Changed);
		UObject* const Owner = Expression.GetOuter();
		if (!Owner)
		{
			OutError.Set(TEXT("material_niagara_graph_owner_type_invalid"));
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
			OutError.Set(TEXT("material_niagara_graph_capture_target_invalid"));
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
				OutError.Set(
					TEXT("material_niagara_graph_item_identity_missing"),
					Expression ? Expression->GetPathName() : TEXT("null"));
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
		FMaterialNiagaraGraphAdapterError& OutError) const
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
				OutError.Set(
					TEXT("material_niagara_graph_parameter_capture_failed"),
					Info.Name.ToString());
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
				OutError.Set(
					TEXT("material_niagara_graph_parameter_capture_failed"),
					Info.Name.ToString());
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
			if (!MaterialInstance.GetTextureParameterValue(
					FHashedMaterialParameterInfo(Info), Value)
				|| !Value)
			{
				OutError.Set(
					TEXT("material_niagara_graph_parameter_capture_failed"),
					Info.Name.ToString());
				return false;
			}
			const FString Ref = Value->GetPathName();
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
				OutError.Set(
					TEXT("material_niagara_graph_parameter_capture_failed"),
					Info.Name.ToString());
				return false;
			}
			Add(ParameterIdentity(Info,
				EMaterialNiagaraGraphParameterType::StaticSwitch),
				IsSwitchOverride(Info), 0.0f, FLinearColor::Black,
				FString(), bValue, bValue ? TEXT("true") : TEXT("false"));
		}
		TArray<FMaterialNiagaraGraphParameterIdentity> Identities;
		for (const FMaterialNiagaraGraphEffectiveParameter& Parameter
			: OutParameters)
		{
			if (!Parameter.IsValid())
			{
				OutError.Set(
					TEXT("material_niagara_graph_parameter_capture_failed"),
					Parameter.GetIdentity().GetParameterKey());
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
		if (UMaterial* Material = Cast<UMaterial>(&Owner))
		{
			UMaterialEditingLibrary::RecompileMaterial(Material);
			OutReceipt.CompileErrorCount =
				CountMaterialCompileErrors(*Material);
		}
		else if (UMaterialInstanceConstant* MaterialInstance =
			Cast<UMaterialInstanceConstant>(&Owner))
		{
			UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
			OutReceipt.CompileErrorCount =
				CountMaterialCompileErrors(*MaterialInstance);
		}
		else if (UMaterialFunction* Function = Cast<UMaterialFunction>(&Owner))
		{
			UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
		}
		else
		{
			OutError.Set(TEXT("material_niagara_graph_compile_target_invalid"));
			return false;
		}
		OutReceipt.bCompilationComplete = true;
		OutReceipt.CompileEvidenceId = NewEvidence(TEXT("compile"), &Owner);
		OutReceipt.SettleEvidenceId = NewEvidence(TEXT("settle"), &Owner);
		if (OutReceipt.CompileErrorCount != 0)
		{
			OutError.Set(
				TEXT("material_niagara_graph_compile_incomplete"),
				FString::FromInt(OutReceipt.CompileErrorCount));
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
		FString NativeError;
		OutModule = AddAssignmentModule(
			*Stage.OutputNode, AssignmentTargets, TargetIndex, NativeError);
		if (!OutModule)
		{
			OutError.Set(
				TEXT("material_niagara_graph_stack_module_add_failed"),
				NativeError);
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
		FString NativeError;
		if (!MoveModule(
				*Stage.OutputNode,
				Module,
				TargetIndex,
				OutNewIndex,
				NativeError))
		{
			OutError.Set(
				TEXT("material_niagara_graph_stack_module_move_failed"),
				NativeError);
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
		GetModules(*Stage.OutputNode, Modules);
		TArray<FMaterialNiagaraGraphItemIdentity> Items;
		TArray<FMaterialNiagaraGraphOrderingIdentity> Ordering;
		FString Predecessor;
		for (int32 Index = 0; Index < Modules.Num(); ++Index)
		{
			UNiagaraNodeFunctionCall* Module = Modules[Index];
			if (!Module || !Module->NodeGuid.IsValid())
			{
				OutError.Set(
					TEXT("material_niagara_graph_item_identity_missing"),
					Module ? Module->GetPathName() : TEXT("null"));
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
			OutError.Set(TEXT("material_niagara_graph_warmup_time_invalid"));
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
			OutError.Set(TEXT("material_niagara_graph_emitter_data_missing"));
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
			OutError.Set(TEXT("material_niagara_graph_script_data_missing"));
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
				OutError.Set(TEXT("material_niagara_graph_compile_incomplete"));
				return false;
			}
			System->CacheFromCompiledData();
			Compile = UeAgentNiagaraCompileValidation::InspectAsset(System);
		}
		else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset))
		{
			UeAgentNiagaraEmitterCompileContext::FResult Result;
			FString NativeError;
			if (!UeAgentNiagaraEmitterCompileContext::Compile(
					*Emitter,
					Emitter->GetExposedVersion().VersionGuid,
					true,
					Result,
					NativeError))
			{
				OutError.Set(
					TEXT("material_niagara_graph_compile_incomplete"),
					NativeError);
				return false;
			}
			Compile = Result.Validation;
		}
		else if (UNiagaraScript* Script = Cast<UNiagaraScript>(&Asset))
		{
			TSet<TWeakObjectPtr<UNiagaraScriptSource>> PreviousCopies;
			CaptureRootedNiagaraCompilationCopies(PreviousCopies);
			Script->RequestCompile(
				Script->GetExposedVersion().VersionGuid,
				true);
			ReleaseNewNiagaraCompilationCopies(PreviousCopies);
			Compile = UeAgentNiagaraCompileValidation::InspectAsset(Script);
		}
		else
		{
			OutError.Set(TEXT("material_niagara_graph_compile_target_invalid"));
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
			OutError.Set(
				TEXT("material_niagara_graph_stack_capture_failed"),
				Stack.FirstError);
			return false;
		}
		if (!OutReceipt.bCompilationComplete
			|| OutReceipt.CompileErrorCount != 0)
		{
			OutError.Set(
				TEXT("material_niagara_graph_compile_incomplete"),
				Compile.FirstError);
			return false;
		}
		if (OutReceipt.StackIssueCount != 0)
		{
			OutError.Set(
				TEXT("material_niagara_graph_stack_issue"),
				Stack.FirstError);
			return false;
		}
		return true;
	}
}
