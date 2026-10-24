#include "Module/CommandRegistration/AssetSimple/UeAgentAssetSimpleOperationRegistration.h"
#include "DomainAdapters/AssetSimple/UeAgentReflectionAssetHjson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"

#include "CommandHandlers/AssetSimple/UeAgentAssetSimpleOperationCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/AssetSimple/UeAgentAssetSimpleMechanicsBoundary.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

namespace UE::UAI::Module::CommandRegistration::AssetSimple
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::AssetSimple;
	using namespace CommandRegistry;
	using namespace Composition::SimpleAuthoring;
	using namespace Module::CommandRegistration::Active;
	using namespace OperationOwners::AssetSimple;

	namespace
	{
		const FString Owner = TEXT("asset-simple-operation-owner");

		FString WireType(const EUeAgentCommandWireType Type)
		{
			switch (Type)
			{
			case EUeAgentCommandWireType::Boolean: return TEXT("boolean");
			case EUeAgentCommandWireType::Integer: return TEXT("integer");
			case EUeAgentCommandWireType::Number: return TEXT("number");
			case EUeAgentCommandWireType::String: return TEXT("string");
			case EUeAgentCommandWireType::Object: return TEXT("object");
			case EUeAgentCommandWireType::Array: return TEXT("array");
			default: return TEXT("any");
			}
		}

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* Type,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, Type, bRequired, false, Description};
		}

		bool TryDeclaration(
			const FUeAgentSimpleAuthoringCommandRoute& Route,
			const FUeAgentCommandContractRegistry& LegacyRegistry,
			FUeAgentCommandDeclaration& OutDeclaration,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* Legacy =
				LegacyRegistry.FindExact(Route.CommandId);
			if (!Legacy || !Legacy->TryValidate(OutError))
			{
				OutError = Legacy
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("asset_simple_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_simple_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}

			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("asset");
			Result.PythonFunction = Route.CommandId.Mid(6).Replace(
				TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& Parameter : Legacy->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(),
					WireType(Parameter.GetWireType()),
					Parameter.IsRequired(),
					Parameter.IsNullable(),
					TEXT("Exact AssetSimple operation parameter consumed by the declared domain mechanics."),
					Parameter.GetSchemaJson(),
				});
			}
			Result.ResultFields = {
				Field(TEXT("operation_id"), TEXT("string"), true,
					TEXT("Stable identifier of the one completed SimpleAuthoring operation.")),
				Field(TEXT("projection"), TEXT("object"), true,
					TEXT("Canonical verified outcome containing status, evidence identities and persisted or current-state readback.")),
			};
			if (Route.CommandId.EndsWith(TEXT(".create")))
			{
				Result.ModelResultFields = {
					{TEXT("status"), {
						TEXT("projection"), TEXT("payload"), TEXT("status")}},
					{TEXT("effect_outcome"), {
						TEXT("projection"), TEXT("payload"),
						TEXT("effect_outcome")}},
				};
			}
			Result.Errors = {
				{TEXT("asset_simple_authority_required"),
					TEXT("The caller does not possess uai:operate authority.")},
				{TEXT("asset_simple_runtime_unavailable"),
					TEXT("The process Operation host cannot bind the current Editor world.")},
				{TEXT("simple_authoring_input_invalid"),
					TEXT("The exact domain schema rejects the request before mutation.")},
				{TEXT("operation_state_unknown"),
					TEXT("A post-mutation failure cannot prove the final state and must not be replayed.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Route.CommandId
				+ TEXT(" through the common OperationService and exact AssetSimple mechanics.");
			Result.Purpose.Target =
				TEXT("One exact persisted asset or project-owned asset setting declared by the command contract.");
			Result.Purpose.Preconditions = {
				TEXT("Use the exact public command identity and contract hash unchanged."),
				TEXT("Resolve all asset and type identities before the first mutation; the server binds the current target revision internally."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use the generated naming SDK method and its public parameters; the SDK performs no domain remapping.");
			Result.Purpose.ResultGuidance =
				TEXT("Treat success as terminal only when projection.payload reports a verified outcome and evidence identities.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures leave no residue; state_unknown is non-replayable and requires reconciliation.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Creates or updates only the exact declared asset target through formal Unreal Engine mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
				TEXT("asset persisted-state query"),
			};
			Result.Purpose.Keywords = {
				TEXT("asset"), TEXT("create"), TEXT("update"), TEXT("OperationService"),
			};
			Result.Purpose.Synonyms = {
				TEXT("simple asset authoring"), TEXT("asset settings mutation"),
			};
			if (Route.CommandId == TEXT("asset.object.create"))
			{
				Result.Purpose.Summary = TEXT(
					"Create a new Unreal Engine asset through one exact catalog constructor.");
				Result.Purpose.Target = TEXT(
					"Any UE asset supported by a registered native factory, static constructor, or domain constructor.");
				Result.Purpose.Preconditions = {
					TEXT("Call asset.factory.candidates with create_command=asset.object.create."),
					TEXT("Choose one returned readable factory name."),
					TEXT("Use a fresh canonical object path target_ref such as /Game/Folder/Asset.Asset; package and object names must match."),
				};
				Result.Purpose.ParameterGuidance = TEXT(
					"Pass one name returned by asset.factory.candidates directly as factory. Pass target_ref as /Game/Folder/Asset.Asset, not the package-only /Game/Folder/Asset form.");
				Result.Purpose.ResultGuidance = TEXT(
					"Use the verified persisted-state projection, then independently unload and reload the created asset before treating it as durable.");
				Result.Purpose.SideEffectGuidance = TEXT(
					"Creates the requested asset through the selected catalog constructor and persists its verified state.");
				Result.Purpose.RelatedFlows = {
					TEXT("query.command.search"),
					TEXT("query.command.usage"),
					TEXT("asset.factory.candidates"),
					TEXT("asset persisted-state query"),
				};
				Result.Purpose.Keywords = {
					TEXT("generic"),
					TEXT("native factory"),
					TEXT("new asset"),
					TEXT("level"),
					TEXT("map"),
					TEXT("material"),
					TEXT("create"),
				};
				Result.Purpose.Synonyms = {
					TEXT("generic asset creation"),
					TEXT("native factory asset creation"),
					TEXT("create level or map"),
					TEXT("create material"),
				};
			}
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			FContribution(
				TArray<FUeAgentCommandDeclaration> InDeclarations,
				TArray<FUeAgentSimpleAuthoringCommandRoute> InRoutes)
				: Declarations(MoveTemp(InDeclarations))
				, Routes(MoveTemp(InRoutes))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("asset-simple-operation");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Append(Declarations);
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return TryCreateAssetSimpleOperationCommandHandlers(
					Routes, OutHandlers, OutError);
			}

		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
			TArray<FUeAgentSimpleAuthoringCommandRoute> Routes;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GAssetSimpleOperationContributionRegistration(
				TEXT("asset-simple-operation"),
				&TryCreateAssetSimpleOperationContribution);

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					TEXT("asset-simple"));
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			for (const FUeAgentSimpleAuthoringCommandRoute& Route
				: GetAssetSimplePublicOperationRoutes())
			{
				const TSharedPtr<const DomainCapability::
					IDomainOperationInvocationFactory> Factory =
						FindOperationInvocationFactory(Route.CommandId);
				if (!Factory.IsValid())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_simple_operation_factory_missing"), TEXT("No operation invocation factory is registered for this command."), TEXT("registration"), FString(), Route.CommandId);
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
					return false;
				}
				DomainCapability::FDomainOperationEntry Operation;
				Operation.OperationId = Route.CommandId;
				Operation.Kind = Route.CommandId.EndsWith(TEXT(".create"))
					? DomainCapability::EDomainOperationKind::Create
					: DomainCapability::EDomainOperationKind::Write;
				Operation.TerminalPolicy = DomainCapability::
					EDomainOperationTerminalPolicy::
						SucceededRejectedRolledBackOrStateUnknown;
				Operation.ExecutionPolicy = DomainCapability::
					EDomainOperationExecutionPolicy::Immediate;
				Operation.TransientStatePolicy = DomainCapability::
					EDomainTransientStatePolicy::None;
				Operation.bPersists = true;
				Operation.bIndependentReadback = true;
				Operation.InvocationFactory = Factory;
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			Declaration.State.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.State.Planes = {
				StateSemantic::EUeAgentStatePlane::AuthoringMemory,
				StateSemantic::EUeAgentStatePlane::Persisted,
			};
			Declaration.Hjson.Support = DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.Support = DomainCapability::EDomainCapabilitySupport::Supported;
			for (const auto& Config : TArray<DomainAdapters::AssetSimple::FReflectionAssetHjsonConfig>{
				{TEXT("pcg_data_asset"), TEXT("/Script/PCG.PCGDataAsset"), TEXT("pcg-data-asset"), {}},
				{TEXT("mesh_deformer_collection"), TEXT("/Script/Engine.MeshDeformerCollection"), TEXT("mesh-deformer-collection"), {}},
				{TEXT("deformer_source"), TEXT("/Script/OptimusCore.OptimusSource"), TEXT("deformer-source"), {}},
				{TEXT("ml_deformer"), TEXT("/Script/MLDeformerFramework.MLDeformerAsset"), TEXT("ml-deformer"), {}},
				{TEXT("material_parameter_collection"), TEXT("/Script/Engine.MaterialParameterCollection"), TEXT("material-parameter-collection"), {}},
				{TEXT("material_function_instance"), TEXT("/Script/Engine.MaterialFunctionInstance"), TEXT("material-function-instance"), {}},
				{TEXT("material_layer_instance"), TEXT("/Script/Engine.MaterialFunctionMaterialLayerInstance"), TEXT("material-layer-instance"), {}},
				{TEXT("material_layer_blend_instance"), TEXT("/Script/Engine.MaterialFunctionMaterialLayerBlendInstance"), TEXT("material-layer-blend-instance"), {}},
				{TEXT("physics_asset"), TEXT("/Script/Engine.PhysicsAsset"), TEXT("physics-asset"), {}},
				{TEXT("static_mesh"), TEXT("/Script/Engine.StaticMesh"), TEXT("static-mesh"), {}},
				{TEXT("skeletal_mesh"), TEXT("/Script/Engine.SkeletalMesh"), TEXT("skeletal-mesh"), {}},
				{TEXT("render_target"), TEXT("/Script/Engine.TextureRenderTarget2D"), TEXT("render-target-2d"), {}},
				{TEXT("runtime_virtual_texture"), TEXT("/Script/Engine.RuntimeVirtualTexture"), TEXT("runtime-virtual-texture"), {}},
				{TEXT("sparse_volume_texture"), TEXT("/Script/Engine.StreamableSparseVolumeTexture"), TEXT("sparse-volume-texture"), {}},
				{TEXT("subuv_animation"), TEXT("/Script/Engine.SubUVAnimation"), TEXT("subuv-animation"), {}},
				{TEXT("texture_collection"), TEXT("/Script/Engine.TextureCollection"), TEXT("texture-collection"), {}},
				{TEXT("texture_2d"), TEXT("/Script/Engine.Texture2D"), TEXT("texture-2d"), {}},
				{TEXT("sound_wave"), TEXT("/Script/Engine.SoundWave"), TEXT("sound-wave"), {}},
				{TEXT("texture_array"), TEXT("/Script/Engine.Texture2DArray"), TEXT("texture-2d-array"), {}},
				{TEXT("texture_cube_array"), TEXT("/Script/Engine.TextureCubeArray"), TEXT("texture-cube-array"), {}},
				{TEXT("texture_cube"), TEXT("/Script/Engine.TextureCube"), TEXT("texture-cube"), {}},
				{TEXT("volume_texture"), TEXT("/Script/Engine.VolumeTexture"), TEXT("volume-texture"), {}},
				{TEXT("data_object"), TEXT("/Script/Engine.DataAsset"), TEXT("data-object"), {}},
				{TEXT("physical_material"), TEXT("/Script/PhysicsCore.PhysicalMaterial"), TEXT("physical-material"), {}},
				{TEXT("input_action"), TEXT("/Script/EnhancedInput.InputAction"), TEXT("input-action"), {TEXT("PlayerMappableKeySettings"), TEXT("Triggers"), TEXT("Modifiers")}},
				{TEXT("input_mapping_context"), TEXT("/Script/EnhancedInput.InputMappingContext"), TEXT("input-mapping-context"), {TEXT("Mappings")}},
				{TEXT("specular_profile"), TEXT("/Script/Engine.SpecularProfile"), TEXT("specular-profile"), {}},
				{TEXT("media_player"), TEXT("/Script/MediaAssets.MediaPlayer"), TEXT("media-player"), {}},
				{TEXT("media_source"), TEXT("/Script/MediaAssets.FileMediaSource"), TEXT("media-source"), {}},
				{TEXT("media_texture"), TEXT("/Script/MediaAssets.MediaTexture"), TEXT("media-texture"), {}},
				{TEXT("paper_flipbook"), TEXT("/Script/Paper2D.PaperFlipbook"), TEXT("paper-flipbook"), {}},
				{TEXT("paper_sprite"), TEXT("/Script/Paper2D.PaperSprite"), TEXT("paper-sprite"), {}},
				{TEXT("paper_sprite_atlas"), TEXT("/Script/Paper2D.PaperSpriteAtlas"), TEXT("paper-sprite-atlas"), {}},
				{TEXT("paper_tileset"), TEXT("/Script/Paper2D.PaperTileSet"), TEXT("paper-tileset"), {}},
				{TEXT("sound_attenuation"), TEXT("/Script/Engine.SoundAttenuation"), TEXT("sound-attenuation"), {}},
				{TEXT("sound_concurrency"), TEXT("/Script/Engine.SoundConcurrency"), TEXT("sound-concurrency"), {}},
				{TEXT("sound_class"), TEXT("/Script/Engine.SoundClass"), TEXT("sound-class"), {}},
				{TEXT("sound_submix"), TEXT("/Script/Engine.SoundSubmix"), TEXT("sound-submix"), {}},
				{TEXT("sound_mix"), TEXT("/Script/Engine.SoundMix"), TEXT("sound-mix"), {}},
				{TEXT("audio_effect_preset"), TEXT("/Script/AudioMixer.SubmixEffectSubmixEQPreset"), TEXT("submix-eq-preset"), {}},
				{TEXT("curve_vector"), TEXT("/Script/Engine.CurveVector"), TEXT("curve-vector"), {}},
				{TEXT("curve_float"), TEXT("/Script/Engine.CurveFloat"), TEXT("curve-float"), {}},
				{TEXT("curve_linear_color"), TEXT("/Script/Engine.CurveLinearColor"), TEXT("curve-linear-color"), {}}})
			{
				const auto Provider = DomainAdapters::AssetSimple::CreateReflectionAssetHjsonProvider(Config);
				const FString Contract = TEXT("uai.init.") + Config.ProfileId + TEXT(".root/1");
				Declaration.Hjson.Providers.Add(Provider);
				Declaration.Hjson.Routes.Add({Provider->GetProviderId(),
					TEXT("content_graph.import_workspace"), TEXT("uai.") + Config.ProfileId + TEXT(".edit/1"),
					HjsonWorkspace::ResolveTargetCompositionKind(FString(), Config.ProfileId), Config.RootType,
					HjsonWorkspace::EInitializationNodeRole::RootTarget, Contract, false});
				Declaration.Initialization.ContractIds.Add(Contract);
			}
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("asset-simple"), &TryCreateDomainCapability);
	}

	bool TryCreateAssetSimpleOperationContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes =
			GetAssetSimplePublicOperationRoutes();
		if (Routes.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_simple_public_routes_empty"), TEXT("No public simple-authoring command routes were produced."), TEXT("registration"));
			return false;
		}
		for (const FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
		{
			if (!DomainAdapters::AssetSimple::
					IsAssetSimpleMechanicsCommand(Route.CommandId))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_simple_public_route_has_no_mechanics"), TEXT("The public route has no registered mechanics implementation."), TEXT("registration"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}
		}
		TSharedPtr<const FUeAgentCommandContractRegistry> LegacyRegistry;
		if (!TryCreateProductionCommandContractRegistry(LegacyRegistry, OutError)
			|| !LegacyRegistry.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		Declarations.Reserve(Routes.Num());
		for (const FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Route, *LegacyRegistry, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(Declarations, Routes);
		return true;
	}

	bool TryCreateAssetSimpleOperationRegistration(
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		return TryCreateAssetSimpleOperationContribution(Contribution, OutError)
			&& Composition.TryAdd(Contribution.ToSharedRef(), OutError)
			&& Composition.TryCompose(OutRegistry, OutDispatch, OutError);
	}
}
