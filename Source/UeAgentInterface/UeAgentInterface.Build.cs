// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UeAgentInterface : ModuleRules
{
	public UeAgentInterface(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PrivateIncludePaths.AddRange(
			new string[]
			{
				System.IO.Path.Combine(EngineDirectory, "Source/Editor/MaterialEditor/Private"),
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"EditorFramework",
				"TypedElementFramework",
				"TypedElementRuntime",
				"ClassViewer",
				"SubobjectDataInterface",
				"PlacementMode",
				"MainFrame",
				"GraphEditor",
				"LevelEditor",
				"ApplicationCore",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"Projects",
				"Settings",
				"EngineSettings",
				"HTTPServer",
				"HTTP",
				"Sockets",
				"Json",
				"JsonUtilities",
				"Localization",
				"ImageCore",
				"AIModule",
				"AIGraph",
				"BehaviorTreeEditor",
				"EnvironmentQueryEditor",
				"GameplayTasks",
				"RenderCore",
				"RHI",
				"Landscape",
				"Foliage",
				"NavigationSystem",
				"AssetRegistry",
				"Kismet",
				"KismetCompiler",
				"BlueprintEditorLibrary",
				"BlueprintGraph",
				"AnimationBlueprintLibrary",
				"AnimationCore",
				"AnimationEditor",
				"AnimationBlueprintEditor",
				"AnimGraph",
				"AdvancedPreviewScene",
				"AssetTools",
				"Persona",
				"StaticMeshEditor",
				"SkeletalMeshEditor",
				"SkeletonEditor",
				"UMG",
				"UMGEditor",
				"EnhancedInput",
				"InputBlueprintNodes",
				"InputCore",
				"PropertyEditor",
				"GameplayTags",
				"StateTreeModule",
				"StateTreeEditorModule",
				"GameplayStateTreeModule",
				"GameplayInteractionsModule",
				"SmartObjectsModule",
				"SmartObjectsEditorModule",
				"WorldConditions",
				"PropertyBindingUtils",
				"PhysicsCore",
				"PhysicsAssetEditor",
				"HairStrandsCore",
				"HairStrandsEditor",
				"MaterialEditor",
				"LevelSequence",
				"LevelSequenceEditor",
				"MovieScene",
				"MovieSceneTools",
				"MovieSceneTracks",
				"Sequencer",
				"SequencerScriptingEditor",
				"ControlRig",
				"ControlRigDeveloper",
				"ControlRigEditor",
				"IKRig",
				"IKRigEditor",
				"FullBodyIK",
				"RigVM",
				"RigVMDeveloper",
				"RigVMEditor",
				"Niagara",
				"NiagaraEditor",
				"PCG",
				"PCGEditor",
				"AlembicImporter",
				"AlembicLibrary",
				"GeometryCache",
				"SparseVolumeTexture",
				"ComputeFramework",
				"OptimusCore",
				"InteractiveToolsFramework",
				"EditorInteractiveToolsFramework",
				"ModelingToolsEditorMode",
				"MeshModelingTools",
				"ModelingComponents",
				"GeometryFramework",
				"GeometryCore",
				"DynamicMesh",
				"GeometryScriptingCore",
				"MeshConversion",
				"MeshDescription",
				"ModelingOperators",
				"ModelingOperatorsEditorOnly",
				"StaticMeshDescription",
				"MediaAssets",
				"Paper2D",
				"TextureGraph",
				"TextureGraphEngine",
				"AudioEditor",
				"AudioMixer",
				"MetasoundEngine",
				"MetasoundEditor",
				"MetasoundFrontend",
				"DataLayerEditor",
				"WorldPartitionEditor",
				"TargetPlatform",
			}
		);

		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "ICU");
	}
}
