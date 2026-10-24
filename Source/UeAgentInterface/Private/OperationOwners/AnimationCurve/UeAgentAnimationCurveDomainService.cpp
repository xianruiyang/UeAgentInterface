#include "OperationOwners/AnimationCurve/UeAgentAnimationCurveDomainService.h"

#include "CommandHandlers/AnimationCurve/UeAgentAnimationCurveDomainCommandHandlers.h"
#include "Dom/JsonValue.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimMetaData.h"
#include "Animation/EditorCompositeSection.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "PersonaUtils.h"
#include "SClassViewer.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWidget.h"
#include "Layout/Children.h"

namespace UE::UAI::OperationOwners::AnimationCurve
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ProviderId = TEXT("animation-curve-domain-provider");
		const FString SchemaId = TEXT("uai.animation_curve.edit/1");
		const FString BlendSpaceType = TEXT("/Script/Engine.BlendSpace");
		const FString BlendSpace1DType = TEXT("/Script/Engine.BlendSpace1D");
		const FString SequenceType = TEXT("/Script/Engine.AnimSequence");
		const FString MontageType = TEXT("/Script/Engine.AnimMontage");
		const FString PoseType = TEXT("/Script/Engine.PoseAsset");
		const FString PoseContract = TEXT("uai.init.animation_curve.pose_asset.root/1");
		const FString MontageContract = TEXT("uai.init.animation_curve.montage.root/1");
		const FString BlendSpaceContract =
			TEXT("uai.init.animation_curve.blend_space.root/1");
		const FString SequenceContract =
			TEXT("uai.init.animation_curve.sequence.root/1");

		bool IsBlendSpaceType(const FString& Type)
		{
			return Type == BlendSpaceType || Type == BlendSpace1DType;
		}

		TSharedPtr<SClassViewer> FindClassPicker(const TSharedRef<SWidget>& Widget)
		{
			if (Widget->GetType() == TEXT("SClassViewer")) return StaticCastSharedRef<SClassViewer>(Widget);
			FChildren* Children = Widget->GetChildren();
			for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
				if (auto Picker = FindClassPicker(Children->GetChildAt(Index))) return Picker;
			return nullptr;
		}

		class FAnimationCurveDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			virtual bool SupportsExplicitCommand(const FString& CommandId) const override
			{
				return CommandId == TEXT("content_graph.import_workspace") || CommandId == TEXT("content_graph.export_workspace");
			}

			virtual bool SupportsNativeOwnedCreationContext(const FString& OwnerType, const FString& PropertyPath) const override
			{
				return ((OwnerType == SequenceType || OwnerType == MontageType) && PropertyPath == TEXT("Notifies"))
					|| (OwnerType == MontageType && PropertyPath == TEXT("CompositeSections"))
					|| ((OwnerType == SequenceType || OwnerType == MontageType || IsBlendSpaceType(OwnerType))
						&& PropertyPath == TEXT("MetaData"));
			}

			virtual bool UsesNativeOwnedFactory(FObjectPropertyBase* Property) const override
			{
				return Property && (Property->GetOwnerStruct() == FAnimNotifyEvent::StaticStruct()
					|| Property->GetOwnerStruct() == FCompositeSection::StaticStruct());
			}

			virtual void FinishNativeOwnedCreation(UObject* Owner) const override
			{
				if (auto* Sequence = Cast<UAnimSequence>(Owner)) Sequence->WaitOnExistingCompression();
			}

			virtual TSharedPtr<SClassViewer> MakeNativeOwnedClassPicker(UObject* Owner, FObjectPropertyBase* Property) const override
			{
				auto* Sequence = Cast<UAnimSequenceBase>(Owner);
				if (!Sequence || !Property || Property->GetOwnerStruct() != FAnimNotifyEvent::StaticStruct()) return nullptr;
				if (Property->GetFName() == TEXT("Notify") && Property->PropertyClass == UAnimNotify::StaticClass())
					return FindClassPicker(PersonaUtils::MakeAnimNotifyPicker(Sequence, FOnClassPicked()));
				if (Property->GetFName() == TEXT("NotifyStateClass") && Property->PropertyClass == UAnimNotifyState::StaticClass())
					return FindClassPicker(PersonaUtils::MakeAnimNotifyStatePicker(Sequence, FOnClassPicked()));
				return nullptr;
			}

			virtual UObject* CreateNativeOwnedDetailsContext(UObject* Owner, FObjectPropertyBase* Property, FString& PropertyPath) const override
			{
				if (!Cast<UAnimMontage>(Owner) || !Property || Property->PropertyClass != UAnimMetaData::StaticClass()) return nullptr;
				auto* Context = NewObject<UEditorCompositeSection>();
				Context->CompositeSection.MetaData.Add(nullptr);
				PropertyPath = TEXT("CompositeSection.MetaData");
				return Context;
			}

			virtual UObject* CreateNativeOwnedObject(UObject* Owner, UClass* Class) const override
			{
				if (Cast<UAnimMontage>(Owner) && Class && Class->IsChildOf(UAnimMetaData::StaticClass()))
					return NewObject<UObject>(Owner, Class, NAME_None, RF_Transactional);
				if (!Cast<UAnimSequenceBase>(Owner) || !Class ||
					(!Class->IsChildOf(UAnimNotify::StaticClass()) && !Class->IsChildOf(UAnimNotifyState::StaticClass()))) return nullptr;
				return NewObject<UObject>(Owner, Class, NAME_None, RF_Transactional);
			}

			virtual const FString& GetProviderId() const override
			{
				return ProviderId;
			}

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request) const override
			{
				const FString& Type = Request.GetRootTypeIdentity();
				return Request.GetRole() == EInitializationNodeRole::RootTarget
					&& (IsBlendSpaceType(Type) || Type == SequenceType || Type == MontageType || Type == PoseType)
					&& (Request.GetCommandId()
							!= TEXT("content_graph.import_workspace")
						|| MatchesDomainImportSchema(Request, SchemaId));
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (!Matches(Request))
				{
					return false;
				}
				OutInitializationContractId =
					IsBlendSpaceType(Request.GetRootTypeIdentity())
						? BlendSpaceContract
						: Request.GetRootTypeIdentity() == MontageType ? MontageContract
						: Request.GetRootTypeIdentity() == PoseType ? PoseContract : SequenceContract;
				return true;
			}

			virtual bool TryExecute(
				const FImportInitializationDomainRequest& Request,
				const FString& InitializationContractId,
				FImportInitializationDomainResponse& OutResponse,
				FWorkspaceError& OutError) const override
			{
				OutResponse = {};
				OutError.Reset();
				const bool bBlend =
					IsBlendSpaceType(Request.GetRootTypeIdentity())
					&& InitializationContractId == BlendSpaceContract;
				const bool bSequence =
					Request.GetRootTypeIdentity() == SequenceType
					&& InitializationContractId == SequenceContract;
				const bool bMontage = Request.GetRootTypeIdentity() == MontageType && InitializationContractId == MontageContract;
				const bool bPose = Request.GetRootTypeIdentity() == PoseType && InitializationContractId == PoseContract;
				if (!bBlend && !bSequence && !bMontage && !bPose)
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_contract_not_executable"),
						TEXT("The initialization contract does not support the requested root type in this domain."), TEXT("domain_execute"));
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.SourceKind = TEXT("native_result");
					Detail.InternalContext = InitializationContractId;
					Detail.PublicMessage += TEXT(" Requested root type: ") + Request.GetRootTypeIdentity() + TEXT(".");
					Detail.PropertyPath = TEXT("initialization_contract_supports_root_type");
					Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					Detail.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.SetFailure(MoveTemp(Detail));
					return false;
				}
				return CommandHandlers::AnimationCurve::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};
	}

	bool TryCreateAnimationCurveDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FAnimationCurveDomainProvider>();
		OutError.Reset();
		return true;
	}
}
