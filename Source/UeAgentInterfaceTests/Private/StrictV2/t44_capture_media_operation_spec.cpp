#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "BehaviorTree/BehaviorTree.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
#include "Components/StateTreeComponentSchema.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaImagePolicy.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelSequence.h"
#include "MetasoundSource.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "MovieScene.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "OperationOwners/CaptureMedia/UeAgentCaptureMediaOwner.h"
#include "Server/UeAgentCommandRegistry.h"
#include "Sound/SoundCue.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UeAgentInterfaceSettings.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"

namespace UE::UAI::Tests::StrictV2::T44
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("capture.image.execute"),
			TEXT("capture.media.execute"),
			TEXT("capture.render.execute"),
		};
		return Commands;
	}

	const TArray<FString>& ExpectedImagePolicies()
	{
		static const TArray<FString> Policies = {
			TEXT("policy:image-capture:anim-blueprint/v1"),
			TEXT("policy:image-capture:anim-sequence/v1"),
			TEXT("policy:image-capture:behavior-tree/v1"),
			TEXT("policy:image-capture:blueprint/v1"),
			TEXT("policy:image-capture:control-rig/v1"),
			TEXT("policy:image-capture:level-sequence/v1"),
			TEXT("policy:image-capture:metasound/v1"),
			TEXT("policy:image-capture:niagara/v1"),
			TEXT("policy:image-capture:pcg-viewport/v1"),
			TEXT("policy:image-capture:sound-cue/v1"),
			TEXT("policy:image-capture:state-tree/v1"),
			TEXT("policy:image-capture:texture/v1"),
			TEXT("policy:image-capture:umg/v1"),
			TEXT("policy:image-capture:viewport-buffer/v1"),
			TEXT("policy:image-capture:viewport/v1"),
		};
		return Policies;
	}

	const TArray<FString>& HistoricalIds()
	{
		static const TArray<FString> Ids = {
			TEXT("anim_blueprint_screenshot"),
			TEXT("anim_sequence_screenshot"),
			TEXT("behavior_tree_screenshot"),
			TEXT("blueprint_screenshot"),
			TEXT("control_rig_screenshot"),
			TEXT("metasound_screenshot"),
			TEXT("niagara_screenshot"),
			TEXT("pcg_screenshot"),
			TEXT("screenshot_viewport"),
			TEXT("screenshot_viewport_buffer"),
			TEXT("sequence_screenshot"),
			TEXT("sound_cue_screenshot"),
			TEXT("state_tree_screenshot"),
			TEXT("texture_screenshot"),
			TEXT("umg_screenshot"),
		};
		return Ids;
	}

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublic(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& ParamsJson)
	{
		const CommandContract::FUeAgentFrozenCommandContract* FrozenContract =
			Registry.IsValid() ? Registry->FindExact(CommandId) : nullptr;
		if (!FrozenContract || !Dispatch.IsValid())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t44-product"), TEXT("req-t44-product"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("t44_public_contract_missing"), CommandId);
		}
		const FString RequestId = TEXT("t44-product-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t44-product\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *FrozenContract->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic) || !Request.IsSet())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t44-product"), RequestId, CommandId,
				FrozenContract->GetContractHash(), Diagnostic.Code, Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("strict-v2-t44-product"),
				{TEXT("uai:query"), TEXT("uai:operate"),
				 TEXT("uai:operation"), TEXT("uai:capture")},
				TEXT("corr-t44-product")),
			MakeShared<FNeverCancelled>());
	}

	FString CanonicalJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		FString Error;
		return CommandContract::Json::TryCanonicalize(Object, Result, Error)
			? Result : FString(TEXT("{}"));
	}

	bool TryOperationRef(
		const CommandEdge::FUeAgentCommandResult& Result,
		FString& OutRefJson,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		OutRefJson.Reset();
		OutPayload.Reset();
		FString Error;
		const TSharedPtr<FJsonObject>* Ref = nullptr;
		return Result.IsOk()
			&& CommandContract::Json::TryParseObject(
				Result.GetPayloadJson(), OutPayload, Error)
			&& OutPayload.IsValid()
			&& OutPayload->TryGetObjectField(TEXT("operation_ref"), Ref)
			&& Ref && Ref->IsValid()
			&& CommandContract::Json::TryCanonicalize(
				(*Ref).ToSharedRef(), OutRefJson, Error);
	}

	FString BuildOperationParams(
		const FString& SubjectRef,
		const FString& PolicyId,
		const TSharedRef<FJsonObject>& Parameters)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("subject_ref"), SubjectRef);
		Root->SetStringField(TEXT("policy_id"), PolicyId);
		Root->SetObjectField(TEXT("parameters"), Parameters);
		return CanonicalJson(Root);
	}

	struct FImagePolicyCase final
	{
		FString Label;
		FString PolicyId;
		FString SubjectRef;
		TSharedPtr<FJsonObject> Parameters;
		FString OutputPath;
	};

	struct FCaptureFixture final
	{
		bool Create(FAutomationTestBase& Test)
		{
			const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Package = CreatePackage(*FString(TEXT("/Game/__UAI_T44/") + Suffix));
			if (!Test.TestNotNull(TEXT("T44 creates one owned fixture package"), Package))
			{
				return false;
			}
			Package->AddToRoot();
			const EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional;

			Blueprint = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), Package, TEXT("BP_T44"), BPTYPE_Normal,
				UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
				TEXT("T44"));
			AnimBlueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
				UAnimInstance::StaticClass(), Package, TEXT("ABP_T44"), BPTYPE_Normal,
				UAnimBlueprint::StaticClass(),
				UAnimBlueprintGeneratedClass::StaticClass(), TEXT("T44")));

			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(
				nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
			if (!Test.TestNotNull(TEXT("T44 loads the real skeletal preview mesh"), Mesh)
				|| !Test.TestNotNull(TEXT("T44 creates a Blueprint fixture"), Blueprint)
				|| !Test.TestNotNull(TEXT("T44 creates an AnimBlueprint fixture"), AnimBlueprint))
			{
				return false;
			}
			AnimBlueprint->TargetSkeleton = Mesh->GetSkeleton();
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);

			AnimSequence = NewObject<UAnimSequence>(
				Package, TEXT("AS_T44"), Flags);
			AnimSequence->SetSkeleton(Mesh->GetSkeleton());
			IAnimationDataController& Controller = AnimSequence->GetController();
			Controller.InitializeModel();
			Controller.SetFrameRate(FFrameRate(30, 1), false);
			Controller.SetNumberOfFrames(FFrameNumber(1), false);
			const FName RootBone = Mesh->GetSkeleton()->GetReferenceSkeleton().GetBoneName(0);
			Controller.AddBoneCurve(RootBone, false);
			Controller.SetBoneTrackKeys(
				RootBone,
				{FVector::ZeroVector, FVector(1.0, 0.0, 0.0)},
				{FQuat::Identity, FQuat::Identity},
				{FVector::OneVector, FVector::OneVector}, false);
			Controller.NotifyPopulated();

			BehaviorTree = NewObject<UBehaviorTree>(Package, TEXT("BT_T44"), Flags);
			UControlRigBlueprintFactory* RigFactory =
				NewObject<UControlRigBlueprintFactory>();
			RigFactory->ParentClass = UControlRig::StaticClass();
			ControlRig = Cast<UControlRigBlueprint>(RigFactory->FactoryCreateNew(
				UControlRigBlueprint::StaticClass(), Package, TEXT("CR_T44"),
				Flags, nullptr, GWarn));
			LevelSequence = NewObject<ULevelSequence>(
				Package, TEXT("LS_T44"), Flags);
			LevelSequence->Initialize();
			LevelSequence->GetMovieScene()->SetDisplayRate(FFrameRate(30, 1));
			LevelSequence->GetMovieScene()->SetTickResolutionDirectly(FFrameRate(30, 1));
			LevelSequence->GetMovieScene()->SetPlaybackRange(0, 2);
			MetaSound = NewObject<UMetaSoundSource>(
				Package, TEXT("MS_T44"), Flags);
			Niagara = NewObject<UNiagaraSystem>(Package, TEXT("NS_T44"), Flags);
			UNiagaraSystemFactoryNew::InitializeSystem(Niagara, true);
			SoundCue = NewObject<USoundCue>(Package, TEXT("SC_T44"), Flags);
			StateTree = NewObject<UStateTree>(Package, TEXT("ST_T44"), Flags);
#if WITH_EDITORONLY_DATA
			UStateTreeEditorData* StateTreeData = NewObject<UStateTreeEditorData>(
				StateTree, UStateTreeEditorData::StaticClass(), NAME_None, RF_Transactional);
			StateTree->EditorData = StateTreeData;
			StateTreeData->Schema = NewObject<UStateTreeComponentSchema>(
				StateTreeData, UStateTreeComponentSchema::StaticClass(),
				NAME_None, RF_Transactional);
			StateTreeData->AddRootState().Name = TEXT("T44Root");
#endif
			Texture = NewObject<UTexture2D>(Package, TEXT("T_T44"), Flags);
			TArray<uint8> TexturePixels;
			TexturePixels.SetNumUninitialized(16 * 16 * 4);
			for (int32 Index = 0; Index < 16 * 16; ++Index)
			{
				TexturePixels[Index * 4 + 0] = 32;
				TexturePixels[Index * 4 + 1] = 96;
				TexturePixels[Index * 4 + 2] = 224;
				TexturePixels[Index * 4 + 3] = 255;
			}
			Texture->Source.Init(
				16, 16, 1, 1, TSF_BGRA8, TexturePixels.GetData());
			Texture->PostEditChange();
			Texture->UpdateResource();

			WidgetBlueprint = Cast<UWidgetBlueprint>(
				FKismetEditorUtilities::CreateBlueprint(
					UUserWidget::StaticClass(), Package, TEXT("WBP_T44"),
					BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
					UWidgetBlueprintGeneratedClass::StaticClass(), TEXT("T44")));
			if (WidgetBlueprint)
			{
				FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
			}

			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (World)
			{
				FActorSpawnParameters Spawn;
				Spawn.ObjectFlags = RF_Transient | RF_TextExportTransient;
				Spawn.SpawnCollisionHandlingOverride =
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				AStaticMeshActor* StaticMeshActor = World->SpawnActor<AStaticMeshActor>(
					AStaticMeshActor::StaticClass(), FTransform::Identity, Spawn);
				PcgActor = StaticMeshActor;
				if (PcgActor)
				{
					UStaticMesh* Cube = LoadObject<UStaticMesh>(
						nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
					if (StaticMeshActor && Cube)
					{
						StaticMeshActor->GetStaticMeshComponent()->SetStaticMesh(Cube);
						StaticMeshActor->GetStaticMeshComponent()->UpdateBounds();
					}
					PcgActor->SetActorLabel(TEXT("T44PcgCaptureActor_") + Suffix);
				}
			}

			const bool bAllAssets = BehaviorTree && ControlRig && LevelSequence
				&& MetaSound && Niagara && SoundCue && StateTree && Texture
				&& WidgetBlueprint && PcgActor;
			if (!Test.TestTrue(TEXT("T44 creates every owned product fixture"), bAllAssets))
			{
				return false;
			}

			OutputRoot = FPaths::ProjectSavedDir()
				/ TEXT("UAI/T44") / Suffix;
			IFileManager::Get().MakeDirectory(*OutputRoot, true);
			BuildCases(Mesh->GetPathName());
			Package->SetDirtyFlag(false);
			return Cases.Num() == 15;
		}

		void Cleanup()
		{
			if (GEditor)
			{
				if (UAssetEditorSubsystem* Editors =
					GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					for (UObject* Asset : Assets())
					{
						if (Asset) Editors->CloseAllEditorsForAsset(Asset);
					}
				}
			}
			if (PcgActor && IsValid(PcgActor))
			{
				PcgActor->Destroy();
			}
			for (const FImagePolicyCase& Case : Cases)
			{
				IFileManager::Get().Delete(*Case.OutputPath, false, true, true);
				IFileManager::Get().Delete(
					*(Case.OutputPath + TEXT(".uai-artifact-owner.json")),
					false, true, true);
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->RemoveFromRoot();
				Package->MarkAsGarbage();
			}
			CollectGarbage(RF_NoFlags, true);
		}

		TArray<UObject*> Assets() const
		{
			return {Blueprint, AnimBlueprint, AnimSequence, BehaviorTree,
				ControlRig, LevelSequence, MetaSound, Niagara, SoundCue,
				StateTree, Texture, WidgetBlueprint};
		}

		void BuildCases(const FString& SkeletalMeshPath)
		{
			auto Add = [this](
				const FString& Label,
				const FString& Policy,
				const FString& Subject,
				const TFunction<void(FJsonObject&)>& Customize)
			{
				FImagePolicyCase Case;
				Case.Label = Label;
				Case.PolicyId = Policy;
				Case.SubjectRef = Subject;
				Case.OutputPath = OutputRoot / (Label + TEXT(".png"));
				Case.Parameters = MakeShared<FJsonObject>();
				Case.Parameters->SetStringField(TEXT("format"), TEXT("png"));
				Case.Parameters->SetNumberField(TEXT("max_size"), 256);
				Case.Parameters->SetStringField(TEXT("output_file"), Case.OutputPath);
				Customize(*Case.Parameters);
				Cases.Add(MoveTemp(Case));
			};
			auto AssetSubject = [](const UObject* Asset)
			{
				return FString(TEXT("capture-subject:asset:"))
					+ Asset->GetPathName();
			};

			Add(TEXT("anim-blueprint"),
				TEXT("policy:image-capture:anim-blueprint/v1"),
				AssetSubject(AnimBlueprint), [](FJsonObject& P)
				{
					P.SetStringField(TEXT("target"), TEXT("graph"));
					P.SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
					P.SetBoolField(TEXT("allow_ui_mutation"), true);
					P.SetBoolField(TEXT("allow_window_mutation"), true);
					P.SetBoolField(TEXT("open_editor_if_needed"), true);
				});
			Add(TEXT("anim-sequence"),
				TEXT("policy:image-capture:anim-sequence/v1"),
				AssetSubject(AnimSequence), [&SkeletalMeshPath](FJsonObject& P)
				{
					P.SetStringField(TEXT("skeletal_mesh_path"), SkeletalMeshPath);
					P.SetNumberField(TEXT("frame_index"), 0);
					P.SetStringField(TEXT("target"), TEXT("viewport"));
				});
			Add(TEXT("behavior-tree"),
				TEXT("policy:image-capture:behavior-tree/v1"),
				AssetSubject(BehaviorTree), [](FJsonObject& P)
				{
					P.SetBoolField(TEXT("open_editor_if_needed"), true);
					P.SetBoolField(TEXT("require_non_black"), false);
				});
			Add(TEXT("blueprint"), TEXT("policy:image-capture:blueprint/v1"),
				AssetSubject(Blueprint), [](FJsonObject& P)
				{
					P.SetStringField(TEXT("target"), TEXT("graph"));
					P.SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
					P.SetBoolField(TEXT("allow_ui_mutation"), true);
					P.SetBoolField(TEXT("allow_window_mutation"), true);
					P.SetBoolField(TEXT("open_editor_if_needed"), true);
				});
			Add(TEXT("control-rig"),
				TEXT("policy:image-capture:control-rig/v1"),
				AssetSubject(ControlRig), [](FJsonObject& P)
				{
					P.SetStringField(TEXT("target"), TEXT("viewport"));
					P.SetBoolField(TEXT("allow_ui_mutation"), true);
					P.SetBoolField(TEXT("allow_window_mutation"), true);
					P.SetBoolField(TEXT("open_editor_if_needed"), true);
				});
			Add(TEXT("level-sequence"),
				TEXT("policy:image-capture:level-sequence/v1"),
				AssetSubject(LevelSequence), [](FJsonObject& P)
				{
					P.SetNumberField(TEXT("frame_index"), 0);
					P.SetNumberField(TEXT("width"), 256);
					P.SetNumberField(TEXT("height"), 144);
					P.SetStringField(TEXT("target"), TEXT("viewport"));
				});
			Add(TEXT("metasound"), TEXT("policy:image-capture:metasound/v1"),
				AssetSubject(MetaSound), [](FJsonObject& P)
				{
					P.SetBoolField(TEXT("require_non_black"), true);
				});
			Add(TEXT("niagara"), TEXT("policy:image-capture:niagara/v1"),
				AssetSubject(Niagara), [](FJsonObject& P)
				{
					P.SetBoolField(TEXT("open_editor_if_needed"), true);
					P.SetStringField(TEXT("target"), TEXT("window"));
				});
			Add(TEXT("pcg-viewport"),
				TEXT("policy:image-capture:pcg-viewport/v1"),
				TEXT("capture-subject:actor:") + PcgActor->GetActorLabel(),
				[](FJsonObject&) {});
			Add(TEXT("sound-cue"), TEXT("policy:image-capture:sound-cue/v1"),
				AssetSubject(SoundCue), [](FJsonObject& P)
				{
					P.SetBoolField(TEXT("require_non_black"), true);
				});
			Add(TEXT("state-tree"), TEXT("policy:image-capture:state-tree/v1"),
				AssetSubject(StateTree), [](FJsonObject& P)
				{
					P.SetBoolField(TEXT("open_editor_if_needed"), true);
					P.SetBoolField(TEXT("require_non_black"), false);
				});
			Add(TEXT("texture"), TEXT("policy:image-capture:texture/v1"),
				AssetSubject(Texture), [](FJsonObject& P)
				{
					P.SetNumberField(TEXT("mip_index"), 0);
					P.SetBoolField(TEXT("require_non_black"), true);
				});
			Add(TEXT("umg"), TEXT("policy:image-capture:umg/v1"),
				AssetSubject(WidgetBlueprint), [](FJsonObject& P)
				{
					P.SetNumberField(TEXT("capture_width"), 256);
					P.SetNumberField(TEXT("capture_height"), 144);
					P.SetStringField(TEXT("crop_mode"), TEXT("full_canvas"));
					P.SetBoolField(TEXT("compile_before_capture"), true);
				});
			Add(TEXT("viewport-buffer"),
				TEXT("policy:image-capture:viewport-buffer/v1"),
				TEXT("capture-subject:viewport:active"), [](FJsonObject& P)
				{
					P.SetStringField(TEXT("buffer"), TEXT("base_color"));
				});
			Add(TEXT("viewport"), TEXT("policy:image-capture:viewport/v1"),
				TEXT("capture-subject:viewport:active"), [](FJsonObject& P)
				{
					P.SetStringField(TEXT("view_mode"), TEXT("lit"));
					P.SetBoolField(TEXT("restore_view_mode_after_capture"), true);
				});
		}

		UPackage* Package = nullptr;
		UBlueprint* Blueprint = nullptr;
		UAnimBlueprint* AnimBlueprint = nullptr;
		UAnimSequence* AnimSequence = nullptr;
		UBehaviorTree* BehaviorTree = nullptr;
		UControlRigBlueprint* ControlRig = nullptr;
		ULevelSequence* LevelSequence = nullptr;
		UMetaSoundSource* MetaSound = nullptr;
		UNiagaraSystem* Niagara = nullptr;
		USoundCue* SoundCue = nullptr;
		UStateTree* StateTree = nullptr;
		UTexture2D* Texture = nullptr;
		UWidgetBlueprint* WidgetBlueprint = nullptr;
		AActor* PcgActor = nullptr;
		FString OutputRoot;
		TArray<FImagePolicyCase> Cases;
	};

	class FCaptureMediaProductLifecycleLatentCommand final
		: public IAutomationLatentCommand
	{
	public:
		FCaptureMediaProductLifecycleLatentCommand(
			FAutomationTestBase* InTest,
			TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> InRegistry,
			TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> InDispatch)
			: Test(InTest)
			, Registry(MoveTemp(InRegistry))
			, Dispatch(MoveTemp(InDispatch))
			, StartedAt(FPlatformTime::Seconds())
		{
		}

		~FCaptureMediaProductLifecycleLatentCommand() override
		{
			Cleanup();
		}

		bool Update() override
		{
			if (!Test || !Registry.IsValid() || !Dispatch.IsValid())
			{
				Cleanup();
				return true;
			}
			if (FPlatformTime::Seconds() - StartedAt > 600.0)
			{
				Test->AddError(FString::Printf(
					TEXT("T44 product lifecycle timed out at stage %d image %d"),
					Stage, ImageIndex));
				Cleanup();
				return true;
			}

			if (Stage == 0)
			{
				UUeAgentInterfaceSettings* Settings =
					GetMutableDefault<UUeAgentInterfaceSettings>();
				if (Settings)
				{
					bOriginalScreenshots = Settings->bEnableScreenshots;
					Settings->bEnableScreenshots = true;
					bSettingsChanged = true;
				}
				if (!Fixture.Create(*Test))
				{
					Cleanup();
					return true;
				}
				Test->TestEqual(TEXT("T44 has one product case per historical row"),
					Fixture.Cases.Num(), 15);
				Stage = 1;
				return false;
			}

			if (Stage == 1)
			{
				if (ImageIndex >= Fixture.Cases.Num())
				{
					Stage = 4;
					return false;
				}
				const FImagePolicyCase& Case = Fixture.Cases[ImageIndex];
				const CommandEdge::FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("capture.image.execute"),
					BuildOperationParams(
						Case.SubjectRef, Case.PolicyId,
						Case.Parameters.ToSharedRef()));
				TSharedPtr<FJsonObject> Payload;
				if (!TryOperationRef(Started, OperationRef, Payload))
				{
					Test->AddError(Case.Label + TEXT(" start failed: ")
						+ Started.GetErrorJson());
					return Finish();
				}
				Stage = 2;
				return false;
			}

			if (Stage == 2)
			{
				TSharedPtr<FJsonObject> Status;
				bool bFinished = false;
				if (!AdvanceToTerminal(
						Fixture.Cases[ImageIndex].Label,
						false, bFinished, Status))
				{
					return Finish();
				}
				if (!bFinished) return false;
				const FImagePolicyCase& Case = Fixture.Cases[ImageIndex];
				if (!Test->TestTrue(
						*FString::Printf(TEXT("%s publishes its requested image artifact"),
							*Case.Label),
						IFileManager::Get().FileSize(*Case.OutputPath) > 0)
					|| !VerifyReceiptAndRelease(Case.Label, Status, true))
				{
					return Finish();
				}
				Test->TestTrue(
					*FString::Printf(TEXT("%s artifact remains independently readable after release"),
						*Case.Label),
					IFileManager::Get().FileSize(*Case.OutputPath) > 0);
				OperationRef.Reset();
				++ImageIndex;
				Stage = 1;
				return false;
			}

			if (Stage == 4)
			{
				const TSharedRef<FJsonObject> Parameters = MakeShared<FJsonObject>();
				Parameters->SetNumberField(TEXT("width"), 64);
				Parameters->SetNumberField(TEXT("height"), 64);
				Parameters->SetNumberField(TEXT("r"), 0.125);
				Parameters->SetNumberField(TEXT("g"), 0.5);
				Parameters->SetNumberField(TEXT("b"), 0.875);
				Parameters->SetNumberField(TEXT("a"), 1.0);
				const CommandEdge::FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("capture.render.execute"),
					BuildOperationParams(
						TEXT("render-resource:operation-local"),
						TEXT("policy:render-operation:clear/v1"), Parameters));
				TSharedPtr<FJsonObject> Payload;
				if (!TryOperationRef(Started, OperationRef, Payload))
				{
					Test->AddError(TEXT("render start failed: ") + Started.GetErrorJson());
					return Finish();
				}
				Stage = 5;
				return false;
			}

			if (Stage == 5)
			{
				TSharedPtr<FJsonObject> Status;
				bool bFinished = false;
				if (!AdvanceToTerminal(TEXT("render-clear"), false, bFinished, Status))
				{
					return Finish();
				}
				if (!bFinished) return false;
				if (!VerifyReceiptAndRelease(TEXT("render-clear"), Status, true))
				{
					return Finish();
				}
				OperationRef.Reset();
				Stage = 6;
				return false;
			}

			if (Stage == 6)
			{
				const TSharedRef<FJsonObject> Parameters = MakeShared<FJsonObject>();
				Parameters->SetNumberField(TEXT("expected_duration"), 10.0);
				const CommandEdge::FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("capture.media.execute"),
					BuildOperationParams(
						TEXT("media-source:audio-submix:master"),
						TEXT("policy:media-recording:submix-wav/v1"), Parameters));
				TSharedPtr<FJsonObject> Payload;
				if (!TryOperationRef(Started, OperationRef, Payload))
				{
					Test->AddError(TEXT("media start failed: ") + Started.GetErrorJson());
					return Finish();
				}
				if (!ResumeOnce(TEXT("media-recording"), Payload))
				{
					return Finish();
				}
				Stage = 7;
				return false;
			}

			if (Stage == 7)
			{
				const CommandEdge::FUeAgentCommandResult Cancelled = SubmitPublic(
					Registry, Dispatch, TEXT("operation.cancel"),
					TEXT("{\"operation_ref\":") + OperationRef
						+ TEXT(",\"reason\":\"t44_product_cleanup\"}"));
				if (!Cancelled.IsOk())
				{
					Test->AddError(TEXT("media cancel failed: ")
						+ Cancelled.GetErrorJson());
					return Finish();
				}
				Stage = 8;
				return false;
			}

			if (Stage == 8)
			{
				TSharedPtr<FJsonObject> Status;
				bool bFinished = false;
				if (!AdvanceToTerminal(
						TEXT("media-cancel"), true, bFinished, Status))
				{
					return Finish();
				}
				if (!bFinished) return false;
				if (!Test->TestTrue(TEXT("media cancellation has a terminal error code"),
						!Status->GetStringField(TEXT("error_code")).IsEmpty())
					|| !VerifyReceiptAndRelease(TEXT("media-cancel"), Status, false))
				{
					return Finish();
				}
				const CommandEdge::FUeAgentCommandResult ReleasedStatus = SubmitPublic(
					Registry, Dispatch, TEXT("operation.status"),
					TEXT("{\"operation_ref\":") + OperationRef + TEXT("}"));
				Test->TestTrue(TEXT("released capture state is unknown and never replayed"),
					!ReleasedStatus.IsOk()
						&& ReleasedStatus.GetErrorJson().Contains(TEXT("operation_not_found")));
				return Finish();
			}

			return Finish();
		}

	private:
		bool AdvanceToTerminal(
			const FString& Label,
			const bool bAllowError,
			bool& OutFinished,
			TSharedPtr<FJsonObject>& OutStatus)
		{
			OutFinished = false;
			const CommandEdge::FUeAgentCommandResult StatusResult = SubmitPublic(
				Registry, Dispatch, TEXT("operation.status"),
				TEXT("{\"operation_ref\":") + OperationRef + TEXT("}"));
			FString IgnoredRef;
			if (!TryOperationRef(StatusResult, IgnoredRef, OutStatus))
			{
				Test->AddError(Label + TEXT(" status failed: ")
					+ StatusResult.GetErrorJson());
				return false;
			}
			if (!OutStatus->GetBoolField(TEXT("terminal")))
			{
				if (!ResumeOnce(Label, OutStatus)) return false;
				if (!OutStatus->GetBoolField(TEXT("terminal"))) return true;
			}
			OutFinished = true;
			FString ErrorCode;
			OutStatus->TryGetStringField(TEXT("error_code"), ErrorCode);
			if (!ErrorCode.IsEmpty() && !bAllowError)
			{
				Test->AddError(Label + TEXT(" terminated with ") + ErrorCode
					+ TEXT(": ") + CanonicalJson(OutStatus.ToSharedRef()));
				return false;
			}
			TrackArtifact(OutStatus);
			return true;
		}

		bool ResumeOnce(
			const FString& Label,
			TSharedPtr<FJsonObject>& InOutStatus)
		{
			if (!InOutStatus.IsValid()) return false;
			const int64 Generation =
				InOutStatus->GetIntegerField(TEXT("generation"));
			const CommandEdge::FUeAgentCommandResult Advanced = SubmitPublic(
				Registry, Dispatch, TEXT("operation.resume"),
				TEXT("{\"expected_generation\":") + LexToString(Generation)
					+ TEXT(",\"operation_ref\":") + OperationRef + TEXT("}"));
			FString IgnoredRef;
			TSharedPtr<FJsonObject> AdvancedStatus;
			if (!TryOperationRef(Advanced, IgnoredRef, AdvancedStatus))
			{
				Test->AddError(Label + TEXT(" resume failed: ")
					+ Advanced.GetErrorJson());
				return false;
			}
			InOutStatus = MoveTemp(AdvancedStatus);
			return true;
		}

		bool VerifyReceiptAndRelease(
			const FString& Label,
			const TSharedPtr<FJsonObject>& Status,
			const bool bRequireArtifact)
		{
			if (bRequireArtifact && !HasReadableArtifact(Status))
			{
				Test->AddError(Label + TEXT(" terminal state has no readable artifact: ")
					+ CanonicalJson(Status.ToSharedRef()));
				return false;
			}
			const FString RefPayload = TEXT("{\"operation_ref\":")
				+ OperationRef + TEXT("}");
			const CommandEdge::FUeAgentCommandResult Receipt = SubmitPublic(
				Registry, Dispatch, TEXT("operation.receipt"), RefPayload);
			TSharedPtr<FJsonObject> ReceiptPayload;
			FString Error;
			if (!Receipt.IsOk()
				|| !CommandContract::Json::TryParseObject(
					Receipt.GetPayloadJson(), ReceiptPayload, Error)
				|| !ReceiptPayload.IsValid()
				|| ReceiptPayload->GetStringField(TEXT("schema"))
					!= TEXT("uai.long_running.receipt.v1"))
			{
				Test->AddError(Label + TEXT(" terminal receipt failed: ")
					+ Receipt.GetErrorJson());
				return false;
			}
			const CommandEdge::FUeAgentCommandResult Released = SubmitPublic(
				Registry, Dispatch, TEXT("operation.release"), RefPayload);
			if (!Released.IsOk())
			{
				Test->AddError(Label + TEXT(" release failed: ")
					+ Released.GetErrorJson());
				return false;
			}
			return true;
		}

		bool HasReadableArtifact(const TSharedPtr<FJsonObject>& Status)
		{
			if (!Status.IsValid()) return false;
			const TSharedPtr<FJsonObject>* Signal = nullptr;
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			const TSharedPtr<FJsonObject>* Artifact = nullptr;
			FString Locator;
			if (!Status->TryGetObjectField(TEXT("latest_signal"), Signal)
				|| !Signal || !Signal->IsValid()
				|| !(*Signal)->TryGetObjectField(TEXT("payload"), Payload)
				|| !Payload || !Payload->IsValid()
				|| !(*Payload)->TryGetObjectField(TEXT("artifact_ref"), Artifact)
				|| !Artifact || !Artifact->IsValid()
				|| !(*Artifact)->TryGetStringField(TEXT("locator"), Locator))
			{
				return false;
			}
			Locator = FPaths::ConvertRelativePathToFull(Locator);
			if (IFileManager::Get().FileSize(*Locator) <= 0) return false;
			PublishedArtifacts.AddUnique(Locator);
			return true;
		}

		void TrackArtifact(const TSharedPtr<FJsonObject>& Status)
		{
			HasReadableArtifact(Status);
		}

		bool Finish()
		{
			Cleanup();
			return true;
		}

		void Cleanup()
		{
			if (bCleaned) return;
			bCleaned = true;
			Fixture.Cleanup();
			for (const FString& Path : PublishedArtifacts)
			{
				IFileManager::Get().Delete(*Path, false, true, true);
				IFileManager::Get().Delete(
					*(Path + TEXT(".uai-artifact-owner.json")),
					false, true, true);
			}
			if (bSettingsChanged)
			{
				if (UUeAgentInterfaceSettings* Settings =
					GetMutableDefault<UUeAgentInterfaceSettings>())
				{
					Settings->bEnableScreenshots = bOriginalScreenshots;
				}
			}
		}

		FAutomationTestBase* Test = nullptr;
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
		FCaptureFixture Fixture;
		TArray<FString> PublishedArtifacts;
		FString OperationRef;
		double StartedAt = 0.0;
		int32 ImageIndex = 0;
		int32 Stage = 0;
		bool bOriginalScreenshots = false;
		bool bSettingsChanged = false;
		bool bCleaned = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T44CaptureMediaOperationTest,
	"UAI.StrictV2.T44.CAPTURE.MEDIA.OPERATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T44CaptureMediaOperationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::DomainCapability;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T44;
	(void)Parameters;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("T44 owns exactly three capture commands"),
		ExpectedCommands().Num(), 3);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has the T44 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("capture-media-job-owner")));
			TestEqual(*FString::Printf(TEXT("%s uses the stable SDK namespace"),
				*CommandId), Contract->GetPythonNamespace(), FString(TEXT("capture")));
		}
	}
	const auto* Lifecycle = Registry->FindExact(TEXT("operation.status"));
	TestNotNull(TEXT("T32 lifecycle remains active"), Lifecycle);
	if (Lifecycle)
	{
		TestEqual(TEXT("T44 does not absorb lifecycle commands"),
			Lifecycle->GetPublicOwner(),
			FString(TEXT("long-running-session-owner")));
	}

	TSharedPtr<const FFrozenDomainCapabilitySet> FrozenCapabilities;
	if (!TestTrue(TEXT("the process domain capability set freezes"),
		TryFreezeProcessDomainCapabilities(FrozenCapabilities, Error))
		|| !FrozenCapabilities.IsValid())
	{
		AddError(Error);
		return false;
	}
	const TSharedRef<const FDomainCapabilityContribution>* Capture =
		FrozenCapabilities->GetContributions().FindByPredicate(
			[](const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId() == TEXT("capture-media-operation");
			});
	TestNotNull(TEXT("T44 owns one typed domain contribution"), Capture);
	if (!Capture)
	{
		return false;
	}
	const FDomainCapabilityDeclaration& Declaration =
		(*Capture)->GetDeclaration();
	TestEqual(TEXT("T44 contributes one current command owner"),
		Declaration.Command.Contributions.Num(), 1);
	TestEqual(TEXT("T44 declares three recoverable long-running operations"),
		Declaration.Operation.Operations.Num(), 3);
	for (const FDomainOperationEntry& Operation
		: Declaration.Operation.Operations)
	{
		TestTrue(TEXT("the operation identity is exact"),
			ExpectedCommands().Contains(Operation.OperationId));
		TestEqual(TEXT("the operation is long-running"),
			Operation.ExecutionPolicy,
			EDomainOperationExecutionPolicy::LongRunning);
		TestEqual(TEXT("the operation has a recoverable session"),
			Operation.TransientStatePolicy,
			EDomainTransientStatePolicy::RecoverableSession);
		TestFalse(TEXT("capture does not declare durable mutation persistence"),
			Operation.bPersists);
		TestTrue(TEXT("capture has independent session/artifact readback"),
			Operation.bIndependentReadback);
		TestTrue(TEXT("each public route has an invocation factory"),
			Operation.InvocationFactory.IsValid());
		if (Operation.InvocationFactory.IsValid())
		{
			TestTrue(TEXT("handler and contribution share one immutable factory"),
				&OperationOwners::CaptureMedia::FindOperationInvocationFactory(
					Operation.OperationId).Get() == Operation.InvocationFactory.Get());
		}
	}
	TestEqual(TEXT("T44 owns one migration identity"),
		Declaration.Migration.OwnerId,
		FString(TEXT("T44-CAPTURE-MEDIA-OPERATION")));

	TArray<FString> ActualPolicies;
	for (const DomainAdapters::CaptureMediaLongJob::
		FUeAgentCaptureMediaImagePolicyDescriptor& Policy
		: DomainAdapters::CaptureMediaLongJob::GetCaptureMediaImagePolicies())
	{
		ActualPolicies.Add(Policy.GetPolicyId());
		TestFalse(TEXT("policy mechanics identities never restore an old public ID"),
			HistoricalIds().Contains(Policy.GetMechanicsId()));
	}
	ActualPolicies.Sort();
	TestEqual(TEXT("T44 exposes exactly the 15 approved image policies"),
		ActualPolicies, ExpectedImagePolicies());
	for (const FString& HistoricalId : HistoricalIds())
	{
		TestTrue(*FString::Printf(TEXT("%s is explicitly non-executable"),
			*HistoricalId),
			UeAgentCommandRegistry::IsExplicitlyUnsupportedCommand(HistoricalId));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T44CaptureMediaProductLifecycleTest,
	"UAI.StrictV2.T44.CAPTURE.MEDIA.OPERATION.PublicProductLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T44CaptureMediaProductLifecycleTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T44;
	(void)Parameters;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the public product composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(
		FCaptureMediaProductLifecycleLatentCommand(this, Registry, Dispatch));
	return true;
}
