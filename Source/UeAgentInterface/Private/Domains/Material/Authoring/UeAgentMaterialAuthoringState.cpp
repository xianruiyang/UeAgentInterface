// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Dom/JsonObject.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Misc/PackageName.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"

namespace UeAgentMaterialAuthoring
{
	namespace
	{
		bool BuildIdentity(
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FString& OutError)
		{
			return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
				ModelId, TargetKind, Selectors, OutIdentity, OutError);
		}

		bool AddReadRequest(
			const FString& SemanticId,
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			const UObject* RootObject,
			const TMap<FString, FString>& Context,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			FString& OutError)
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = SemanticId;
			if (!BuildIdentity(ModelId, TargetKind, Selectors, Request.TargetIdentity, OutError)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = RootObject;
			Request.Context = Context;
			OutRequests.Add(MoveTemp(Request));
			return true;
		}

		UObject* ResolveRequestObject(const UeAgentAuthoringState::FReadRequest& Request)
		{
			FString ResolveError;
			UObject* Object = UeAgentAuthoringState::ResolveRequestRootObject(Request, ResolveError);
			if (!IsValid(Object) || GetAssetKind(Object).IsEmpty()) return nullptr;
			return Object;
		}

		TSharedRef<FJsonObject> BuildExportTargetState(const UObject& Asset, const FString& AssetPath)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("asset_path"), AssetPath);
			State->SetStringField(TEXT("asset_kind"), GetAssetKind(&Asset));
			if (const UMaterialInstance* Instance =
				Cast<UMaterialInstance>(&Asset))
			{
				State->SetStringField(
					TEXT("parent_path"),
					GetPathNameSafe(Instance->Parent));
			}
			return State;
		}

		TSharedRef<FJsonObject> BuildInstanceParameterState(
			const UeAgentAuthoringState::FReadRequest& Request)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			for (const TCHAR* Field : {
				TEXT("parameter_kind"),
				TEXT("parameter_name"),
				TEXT("association"),
				TEXT("parameter_index")})
			{
				State->SetStringField(
					Field,
					Request.Context.FindRef(Field));
			}
			const FString Kind =
				Request.Context.FindRef(TEXT("parameter_kind"));
			if (Kind == TEXT("scalar"))
			{
				State->SetNumberField(
					TEXT("value"),
					FCString::Atod(
						*Request.Context.FindRef(TEXT("value"))));
			}
			else if (Kind == TEXT("vector"))
			{
				TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				for (const TCHAR* Channel : {
					TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")})
				{
					Value->SetNumberField(
						Channel,
						FCString::Atod(
							*Request.Context.FindRef(Channel)));
				}
				State->SetObjectField(TEXT("value"), Value);
			}
			else
			{
				State->SetStringField(
					TEXT("value"),
					Request.Context.FindRef(TEXT("value")));
			}
			State->SetBoolField(TEXT("authored_override"), true);
			return State;
		}

		TSharedRef<FJsonObject> BuildExportExpressionState(const UMaterialExpression& Expression)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("expression_class"), Expression.GetClass()->GetPathName());
			TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), Expression.MaterialExpressionEditorX);
			Position->SetNumberField(TEXT("y"), Expression.MaterialExpressionEditorY);
			State->SetObjectField(TEXT("pos"), Position);
			if (const UMaterialExpressionMaterialFunctionCall* FunctionCall =
				Cast<UMaterialExpressionMaterialFunctionCall>(&Expression))
			{
				if (FunctionCall->MaterialFunction)
				{
					State->SetStringField(
						TEXT("material_function_path"), FunctionCall->MaterialFunction->GetPathName());
				}
			}
			if (const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(&Expression))
			{
				TArray<TSharedPtr<FJsonValue>> Inputs;
				for (const FCustomInput& Input : Custom->Inputs)
				{
					Inputs.Add(MakeShared<FJsonValueString>(Input.InputName.ToString()));
				}
				if (!Inputs.IsEmpty()) State->SetArrayField(TEXT("custom_inputs"), Inputs);
			}
			return State;
		}

		TSharedRef<FJsonObject> BuildExportEdgeState(
			const UeAgentAuthoringState::FReadRequest& Request)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
			From->SetStringField(TEXT("node"), Request.Context.FindRef(TEXT("from_guid")));
			From->SetStringField(TEXT("output"), Request.Context.FindRef(TEXT("output")));
			TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
			To->SetStringField(TEXT("node"), Request.Context.FindRef(TEXT("to_guid")));
			To->SetStringField(TEXT("input"), Request.Context.FindRef(TEXT("input")));
			State->SetObjectField(TEXT("from"), From);
			State->SetObjectField(TEXT("to"), To);
			return State;
		}

		TSharedRef<FJsonObject> BuildExportRootInputState(
			const UeAgentAuthoringState::FReadRequest& Request)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
			From->SetStringField(TEXT("node"), Request.Context.FindRef(TEXT("from_guid")));
			From->SetStringField(TEXT("output"), Request.Context.FindRef(TEXT("output")));
			State->SetObjectField(TEXT("from"), From);
			return State;
		}

		TSharedRef<FJsonObject> BuildAssetState(
			const UObject& Asset,
			const FString& CanonicalAssetPath,
			FString& OutError)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(
				TEXT("asset_path"),
				CanonicalAssetPath.IsEmpty() ? Asset.GetOutermost()->GetName() : CanonicalAssetPath);
			State->SetStringField(TEXT("asset_kind"), GetAssetKind(&Asset));
			if (const UMaterialFunction* Function = Cast<UMaterialFunction>(&Asset))
			{
				State->SetStringField(TEXT("description"), Function->Description);
				State->SetBoolField(TEXT("expose_to_library"), Function->bExposeToLibrary);
			}
			TSharedPtr<FJsonObject> Graph;
			if (!BuildGraphState(Asset, Graph, OutError)) return State;
			State->SetObjectField(TEXT("graph"), Graph);
			return State;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentAuthoringState::FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			OutValue.bEditable = false;
			OutValue.Provenance = StateAccessorId;
			OutError.Reset();

			const FString* ExpectedAbsent = Request.Context.Find(TEXT("expected_absent"));
			if (ExpectedAbsent && ExpectedAbsent->Equals(TEXT("true"), ESearchCase::IgnoreCase))
			{
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}

			if (Request.SemanticId == ExportExpressionSemanticId)
			{
				const UMaterialExpression* Expression = Cast<UMaterialExpression>(Request.RootObject);
				if (!Expression)
				{
					OutError = TEXT("material_export_expression_target_invalid");
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildExportExpressionState(*Expression));
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}
			if (Request.SemanticId
				== ExportInstanceParameterSemanticId)
			{
				OutValue.Value = MakeShared<FJsonValueObject>(
					BuildInstanceParameterState(Request));
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}
			if (Request.SemanticId == ExportEdgeSemanticId)
			{
				OutValue.Value = MakeShared<FJsonValueObject>(BuildExportEdgeState(Request));
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}
			if (Request.SemanticId == ExportRootInputSemanticId)
			{
				OutValue.Value = MakeShared<FJsonValueObject>(BuildExportRootInputState(Request));
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			UObject* Object = ResolveRequestObject(Request);
			if (!Object)
			{
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}
			const FString ExpectedKind = Request.SemanticId == MaterialSemanticId
				? TEXT("material")
				: Request.SemanticId == MaterialFunctionSemanticId
					|| Request.SemanticId == ExportFunctionTargetSemanticId
					|| Request.SemanticId == ExportFunctionSettingsSemanticId
					? TEXT("material_function")
					: Request.SemanticId == MaterialInstanceSemanticId
						|| Request.SemanticId
							== ExportInstanceTargetSemanticId
						? TEXT("material_instance")
					: Request.SemanticId == ExportTargetSemanticId
						? TEXT("material")
						: FString();
			if (ExpectedKind.IsEmpty() || GetAssetKind(Object) != ExpectedKind)
			{
				OutError = TEXT("material_state_target_type_invalid");
				return UeAgentAuthoringState::EReadOutcome::Failed;
			}
			const FString AssetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
			TSharedRef<FJsonObject> State =
				Request.SemanticId == ExportTargetSemanticId
					|| Request.SemanticId == ExportFunctionTargetSemanticId
					|| Request.SemanticId
						== ExportInstanceTargetSemanticId
				? BuildExportTargetState(*Object, AssetPath)
				: Request.SemanticId == ExportFunctionSettingsSemanticId
					? [&Object]()
					{
						TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
						const UMaterialFunction* Function = CastChecked<UMaterialFunction>(Object);
						Settings->SetStringField(TEXT("description"), Function->Description);
						Settings->SetBoolField(TEXT("expose_to_library"), Function->bExposeToLibrary);
						return Settings;
					}()
					: BuildAssetState(*Object, AssetPath, OutError);
			if (!OutError.IsEmpty()) return UeAgentAuthoringState::EReadOutcome::Failed;
			OutValue.Value = MakeShared<FJsonValueObject>(State);
			OutValue.bAvailable = true;
			return UeAgentAuthoringState::EReadOutcome::Resolved;
		}

		UeAgentAuthoringNativeDefaults::EProbeOutcome ProbeNativeDefault(
			const UeAgentAuthoringNativeDefaults::FRequest& Request,
			UeAgentAuthoringNativeDefaults::FScratchScope& Scratch,
			UeAgentAuthoringNativeDefaults::FProbeValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
			OutError.Reset();
			const FString TargetKind = Request.TargetIdentity.TargetKind;
			const FString AssetKind = TargetKind == TEXT("material_asset")
				? TEXT("material")
				: TargetKind == TEXT("material_function")
					? TEXT("material_function")
					: FString();
			if (AssetKind.IsEmpty()) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
			UPackage* Package = Scratch.CreateScratchPackage(OutError);
			if (!Package) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			UObject* Seed = CreateFactorySeed(
				AssetKind, Package, NAME_None, RF_Transactional, OutError);
			if (!Seed) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			TSharedRef<FJsonObject> State = BuildAssetState(*Seed, FString(), OutError);
			if (!OutError.IsEmpty()) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			OutValue.Value = MakeShared<FJsonValueObject>(State);
			OutValue.Provenance = FactoryDefaultProviderId;
			return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
		}
	}

	UObject* CreateFactorySeed(
		const FString& AssetKind,
		UObject* Outer,
		const FName Name,
		const EObjectFlags Flags,
		FString& OutError)
	{
		OutError.Reset();
		if (!Outer)
		{
			OutError = TEXT("material_factory_outer_missing");
			return nullptr;
		}
		if (AssetKind == TEXT("material"))
		{
			UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
			UObject* Result = Factory->FactoryCreateNew(
				UMaterial::StaticClass(), Outer, Name, Flags, nullptr, GWarn);
			if (!Result) OutError = TEXT("material_factory_create_failed");
			return Result;
		}
		if (AssetKind == TEXT("material_function"))
		{
			UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
			UObject* Result = Factory->FactoryCreateNew(
				UMaterialFunction::StaticClass(), Outer, Name, Flags, nullptr, GWarn);
			if (!Result) OutError = TEXT("material_function_factory_create_failed");
			return Result;
		}
		OutError = TEXT("material_factory_kind_invalid:") + AssetKind;
		return nullptr;
	}

	bool BuildAssetSnapshotReadRequest(
		UObject* Asset,
		const FString& CanonicalAssetPath,
		const FString& AssetKind,
		const bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError)
	{
		OutError.Reset();
		FString Kind = GetAssetKind(Asset);
		if (Kind.IsEmpty()) Kind = AssetKind;
		const FString SemanticId = Kind == TEXT("material")
			? MaterialSemanticId
			: Kind == TEXT("material_function")
				? MaterialFunctionSemanticId
				: FString();
		const FString IdentityModelId = Kind == TEXT("material")
			? MaterialIdentityModelId
			: Kind == TEXT("material_function")
				? MaterialFunctionIdentityModelId
				: FString();
		const FString TargetKind = Kind == TEXT("material")
			? TEXT("material_asset")
			: Kind == TEXT("material_function")
				? TEXT("material_function")
				: FString();
		if (SemanticId.IsEmpty() || CanonicalAssetPath.IsEmpty())
		{
			OutError = TEXT("material_snapshot_identity_invalid");
			return false;
		}
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(
			IdentityModelId,
			TargetKind,
			{ { TEXT("asset_path"), CanonicalAssetPath } },
			Identity,
			OutError))
		{
			return false;
		}
		UeAgentAuthoringState::FReadRequest& Request = OutRequests.AddDefaulted_GetRef();
		Request.SemanticId = SemanticId;
		Request.TargetIdentity = MoveTemp(Identity);
		Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
		Request.TargetQuery.TargetKind = TargetKind;
		Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
		Request.RootObject = Asset;
		Request.RootOwnerPath = CanonicalAssetPath;
		if (bExpectedAbsent) Request.Context.Add(TEXT("expected_absent"), TEXT("true"));
		return true;
	}

	bool BuildAssetExportSnapshotReadRequests(
		UObject& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError)
	{
		OutRequests.Reset();
		OutError.Reset();
		const FString AssetKind = GetAssetKind(&Asset);
		const FString AssetPath = Asset.GetOutermost()->GetName();
		const bool bFunction = AssetKind == TEXT("material_function");
		const bool bInstance = AssetKind == TEXT("material_instance");
		if (AssetKind != TEXT("material") && !bFunction && !bInstance)
		{
			OutError = TEXT("material_export_asset_type_invalid");
			return false;
		}

		const FString AssetModel = bFunction
			? MaterialFunctionIdentityModelId
			: bInstance
				? MaterialInstanceIdentityModelId
				: MaterialIdentityModelId;
		const FString AssetTargetKind = bFunction
			? TEXT("material_function")
			: bInstance
				? TEXT("material_instance")
				: TEXT("material_asset");
		const FString TargetSemantic = bFunction
			? ExportFunctionTargetSemanticId
			: bInstance
				? ExportInstanceTargetSemanticId
				: ExportTargetSemanticId;
		if (!AddReadRequest(
			TargetSemantic, AssetModel, AssetTargetKind,
			{ { TEXT("asset_path"), AssetPath } }, &Asset, {}, OutRequests, OutError))
		{
			return false;
		}

		UeAgentAuthoringState::FEditablePropertyReadOptions OwnerPropertyOptions;
		OwnerPropertyOptions.SemanticId = TEXT("core.property");
		if (bFunction)
		{
			OwnerPropertyOptions.ExcludedProperties = { TEXT("Description"), TEXT("bExposeToLibrary") };
		}
		else if (bInstance)
		{
			OwnerPropertyOptions.ExcludedProperties = {
				TEXT("ScalarParameterValues"),
				TEXT("VectorParameterValues"),
				TEXT("DoubleVectorParameterValues"),
				TEXT("TextureParameterValues"),
				TEXT("TextureCollectionParameterValues"),
				TEXT("RuntimeVirtualTextureParameterValues"),
				TEXT("SparseVolumeTextureParameterValues"),
				TEXT("FontParameterValues")};
		}
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
			&Asset, OwnerPropertyOptions, OutRequests, OutError))
		{
			return false;
		}

		if (bInstance)
		{
			UMaterialInstance* Instance = CastChecked<UMaterialInstance>(&Asset);
			auto AddParameter = [
				&AssetPath,
				&Asset,
				&OutRequests,
				&OutError](
					const FString& Kind,
					const FMaterialParameterInfo& Info,
					TMap<FString, FString> Context)
			{
				const FString Association = FString::FromInt(
					static_cast<int32>(Info.Association));
				const FString Index = FString::FromInt(Info.Index);
				Context.Add(TEXT("parameter_kind"), Kind);
				Context.Add(
					TEXT("parameter_name"),
					Info.Name.ToString());
				Context.Add(TEXT("association"), Association);
				Context.Add(TEXT("parameter_index"), Index);
				return AddReadRequest(
					ExportInstanceParameterSemanticId,
					MaterialInstanceParameterIdentityModelId,
					TEXT("material_instance_parameter"),
					{
						{TEXT("asset_path"), AssetPath},
						{TEXT("parameter_kind"), Kind},
						{TEXT("parameter_name"), Info.Name.ToString()},
						{TEXT("association"), Association},
						{TEXT("parameter_index"), Index}},
					&Asset,
					Context,
					OutRequests,
					OutError);
			};
			for (const FScalarParameterValue& Parameter
				: Instance->ScalarParameterValues)
			{
				if (!AddParameter(
					TEXT("scalar"),
					Parameter.ParameterInfo,
					{{TEXT("value"), FString::SanitizeFloat(
						Parameter.ParameterValue)}}))
				{
					return false;
				}
			}
			for (const FVectorParameterValue& Parameter
				: Instance->VectorParameterValues)
			{
				const FLinearColor& Value = Parameter.ParameterValue;
				if (!AddParameter(
					TEXT("vector"),
					Parameter.ParameterInfo,
					{
						{TEXT("r"), FString::SanitizeFloat(Value.R)},
						{TEXT("g"), FString::SanitizeFloat(Value.G)},
						{TEXT("b"), FString::SanitizeFloat(Value.B)},
						{TEXT("a"), FString::SanitizeFloat(Value.A)}}))
				{
					return false;
				}
			}
			for (const FTextureParameterValue& Parameter
				: Instance->TextureParameterValues)
			{
				if (!AddParameter(
					TEXT("texture"),
					Parameter.ParameterInfo,
					{{TEXT("value"), GetPathNameSafe(
						Parameter.ParameterValue)}}))
				{
					return false;
				}
			}
			return true;
		}

		if (bFunction && !AddReadRequest(
			ExportFunctionSettingsSemanticId,
			MaterialFunctionIdentityModelId,
			TEXT("material_function"),
			{ { TEXT("asset_path"), AssetPath } },
			&Asset,
			{},
			OutRequests,
			OutError))
		{
			return false;
		}

		TSharedPtr<FJsonObject> GraphState;
		if (!BuildGraphState(Asset, GraphState, OutError) || !GraphState.IsValid()) return false;
		const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
		if (!GraphState->TryGetArrayField(TEXT("expressions"), Expressions) || !Expressions)
		{
			OutError = TEXT("material_export_expression_state_missing");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Expressions)
		{
			const TSharedPtr<FJsonObject>* ExpressionState = nullptr;
			FString GuidText;
			FGuid Guid;
			if (!Value.IsValid() || !Value->TryGetObject(ExpressionState)
				|| !ExpressionState || !ExpressionState->IsValid()
				|| !(*ExpressionState)->TryGetStringField(TEXT("expression_guid"), GuidText)
				|| !FGuid::Parse(GuidText, Guid))
			{
				OutError = TEXT("material_export_expression_identity_invalid");
				return false;
			}
			FExpressionSelector Selector;
			Selector.ExpressionGuid = Guid;
			UMaterialExpression* Expression = ResolveExpressionUnique(Asset, Selector, OutError);
			if (!Expression) return false;
			if (!AddReadRequest(
				ExportExpressionSemanticId,
				MaterialExpressionIdentityModelId,
				TEXT("material_expression"),
				{ { TEXT("asset_path"), AssetPath }, { TEXT("expression_guid"), GuidText } },
				Expression,
				{},
				OutRequests,
				OutError))
			{
				return false;
			}

			UeAgentAuthoringState::FEditablePropertyReadOptions PropertyOptions;
			PropertyOptions.SemanticId = ExportExpressionPropertySemanticId;
			PropertyOptions.ExcludedProperties = {
				TEXT("MaterialFunction"), TEXT("Inputs"), TEXT("MaterialExpressionGuid")
			};
			PropertyOptions.BuildSnapshotIdentity =
				[AssetPath, GuidText](
					const FString& PropertyPath,
					UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
					FString& IdentityError)
				{
					return BuildIdentity(
						MaterialExpressionPropertyIdentityModelId,
						TEXT("material_expression_property"),
						{ { TEXT("asset_path"), AssetPath },
							{ TEXT("expression_guid"), GuidText },
							{ TEXT("property_path"), PropertyPath } },
						OutIdentity,
						IdentityError);
				};
			if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
				Expression, PropertyOptions, OutRequests, OutError))
			{
				return false;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (GraphState->TryGetArrayField(TEXT("edges"), Edges) && Edges)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Edges)
			{
				const TSharedPtr<FJsonObject>* Edge = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Edge) || !Edge || !Edge->IsValid())
				{
					OutError = TEXT("material_export_edge_state_invalid");
					return false;
				}
				TMap<FString, FString> Context;
				for (const TCHAR* Field : { TEXT("from_guid"), TEXT("output"), TEXT("to_guid"), TEXT("input") })
				{
					FString FieldValue;
					(*Edge)->TryGetStringField(Field, FieldValue);
					Context.Add(Field, FieldValue);
				}
				const FString EdgeKey = Context[TEXT("from_guid")] + TEXT(":") + Context[TEXT("output")]
					+ TEXT("->") + Context[TEXT("to_guid")] + TEXT(":") + Context[TEXT("input")];
				if (!AddReadRequest(
					ExportEdgeSemanticId, MaterialEdgeIdentityModelId, TEXT("material_edge"),
					{ { TEXT("asset_path"), AssetPath }, { TEXT("edge_key"), EdgeKey } },
					&Asset, Context, OutRequests, OutError))
				{
					return false;
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* RootInputs = nullptr;
		if (GraphState->TryGetArrayField(TEXT("root_inputs"), RootInputs) && RootInputs)
		{
			for (const TSharedPtr<FJsonValue>& Value : *RootInputs)
			{
				const TSharedPtr<FJsonObject>* RootInput = nullptr;
				FString MaterialProperty;
				TMap<FString, FString> Context;
				if (!Value.IsValid() || !Value->TryGetObject(RootInput)
					|| !RootInput || !RootInput->IsValid()
					|| !(*RootInput)->TryGetStringField(TEXT("material_property"), MaterialProperty))
				{
					OutError = TEXT("material_export_root_input_state_invalid");
					return false;
				}
				FString FromGuid;
				FString Output;
				(*RootInput)->TryGetStringField(TEXT("from_guid"), FromGuid);
				(*RootInput)->TryGetStringField(TEXT("output"), Output);
				Context.Add(TEXT("from_guid"), FromGuid);
				Context.Add(TEXT("output"), Output);
				if (!AddReadRequest(
					ExportRootInputSemanticId,
					MaterialRootInputIdentityModelId,
					TEXT("material_root_input"),
					{ { TEXT("asset_path"), AssetPath }, { TEXT("material_property"), MaterialProperty } },
					&Asset,
					Context,
					OutRequests,
					OutError))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool RegisterStateAccessor(FString& OutError)
	{
		UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
		Descriptor.AccessorId = StateAccessorId;
		Descriptor.TargetKinds = {
			TEXT("material_asset"), TEXT("material_function"), TEXT("material_instance"),
			TEXT("material_instance_parameter"), TEXT("material_expression"),
			TEXT("material_edge"), TEXT("material_root_input")
		};
		Descriptor.CapabilityTags = { TEXT("asset"), TEXT("graph"), TEXT("expression") };
		UeAgentAuthoringState::ERegistrationResult Result;
		return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
			Descriptor, ReadState, Result, OutError);
	}

	bool RegisterNativeDefaultProvider(FString& OutError)
	{
		UeAgentAuthoringNativeDefaults::FProviderDescriptor Descriptor;
		Descriptor.ProviderId = FactoryDefaultProviderId;
		Descriptor.SourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory;
		Descriptor.TargetKinds = { TEXT("material_asset"), TEXT("material_function") };
		Descriptor.CapabilityTags = { TEXT("factory"), TEXT("asset"), TEXT("graph") };
		Descriptor.bRequiresScratch = true;
		UeAgentAuthoringNativeDefaults::ERegistrationResult Result;
		return UeAgentAuthoringNativeDefaults::FProviderRegistry::Get().RegisterProvider(
			Descriptor, ProbeNativeDefault, Result, OutError);
	}
}
