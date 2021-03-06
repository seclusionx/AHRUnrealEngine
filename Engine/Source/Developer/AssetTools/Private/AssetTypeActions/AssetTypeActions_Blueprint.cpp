// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "AssetToolsPrivatePCH.h"
#include "PackageTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "BlueprintEditorModule.h"
#include "BlueprintEditor.h"
#include "AssetRegistryModule.h"
#include "SBlueprintDiff.h"
#include "ISourceControlModule.h"
#include "MessageLog.h"
#include "Engine/BlueprintGeneratedClass.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions"

void FAssetTypeActions_Blueprint::GetActions( const TArray<UObject*>& InObjects, FMenuBuilder& MenuBuilder )
{
	auto Blueprints = GetTypedWeakObjectPtrs<UBlueprint>(InObjects);
	
	if (Blueprints.Num() > 1)
	{
		// Ensure that all the selected blueprints are actors
		bool bCanEditSharedDefaults = true;
		for (auto Blueprint : Blueprints)
		{
			if (!Blueprint.Get()->ParentClass->IsChildOf(AActor::StaticClass()))
			{
				bCanEditSharedDefaults = false;
				break;
			}
		}

		if (bCanEditSharedDefaults)
		{
			MenuBuilder.AddMenuEntry(
			LOCTEXT("Blueprint_EditDefaults", "Edit Shared Defaults"),
			LOCTEXT("Blueprint_EditDefaultsTooltip", "Edit the shared default properties of the selected blueprints."),
			FSlateIcon(FEditorStyle::GetStyleSetName(), "Kismet.Tabs.BlueprintDefaults"),
			FUIAction(
				FExecuteAction::CreateSP( this, &FAssetTypeActions_Blueprint::ExecuteEditDefaults, Blueprints ),
				FCanExecuteAction()
				)
			);
		}	
	}

	if ( Blueprints.Num() == 1 && CanCreateNewDerivedBlueprint() )
	{
		TAttribute<FText>::FGetter DynamicTooltipGetter;
		DynamicTooltipGetter.BindSP(this, &FAssetTypeActions_Blueprint::GetNewDerivedBlueprintTooltip, Blueprints[0]);
		TAttribute<FText> DynamicTooltipAttribute = TAttribute<FText>::Create(DynamicTooltipGetter);

		MenuBuilder.AddMenuEntry(
			LOCTEXT("Blueprint_NewDerivedBlueprint", "Create Blueprint based on this"),
			DynamicTooltipAttribute,
			FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.CreateClassBlueprint"),
			FUIAction(
				FExecuteAction::CreateSP( this, &FAssetTypeActions_Blueprint::ExecuteNewDerivedBlueprint, Blueprints[0] ),
				FCanExecuteAction::CreateSP( this, &FAssetTypeActions_Blueprint::CanExecuteNewDerivedBlueprint, Blueprints[0] )
				)
			);
	}
}

void FAssetTypeActions_Blueprint::OpenAssetEditor( const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor )
{
	EToolkitMode::Type Mode = EditWithinLevelEditor.IsValid() ? EToolkitMode::WorldCentric : EToolkitMode::Standalone;

	for (auto ObjIt = InObjects.CreateConstIterator(); ObjIt; ++ObjIt)
	{
		auto Blueprint = Cast<UBlueprint>(*ObjIt);
		if (Blueprint && Blueprint->SkeletonGeneratedClass && Blueprint->GeneratedClass )
		{
			FBlueprintEditorModule& BlueprintEditorModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>( "Kismet" );
			TSharedRef< IBlueprintEditor > NewKismetEditor = BlueprintEditorModule.CreateBlueprintEditor( Mode, EditWithinLevelEditor, Blueprint, ShouldUseDataOnlyEditor(Blueprint) );
		}
		else
		{
			FMessageDialog::Open( EAppMsgType::Ok, LOCTEXT("FailedToLoadBlueprint", "Blueprint could not be loaded because it derives from an invalid class.  Check to make sure the parent class for this blueprint hasn't been removed!"));
		}
	}
}

bool FAssetTypeActions_Blueprint::CanMerge() const
{
	return true;
}

void FAssetTypeActions_Blueprint::Merge(UObject* InObject)
{
	UBlueprint* AsBlueprint = CastChecked<UBlueprint>(InObject);
	// Kludge to get the merge panel in the blueprint editor to show up:
	bool Success = FAssetEditorManager::Get().OpenEditorForAsset(InObject);
	if( Success )
	{
		FBlueprintEditorModule& BlueprintEditorModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>( "Kismet" );

		FBlueprintEditor* BlueprintEditor = static_cast<FBlueprintEditor*>(FAssetEditorManager::Get().FindEditorForAsset(AsBlueprint, false));
		BlueprintEditor->CreateMergeToolTab();
	}
}

void FAssetTypeActions_Blueprint::Merge(UObject* BaseAsset, UObject* RemoteAsset, UObject* LocalAsset, const FOnMergeResolved& ResolutionCallback)
{
	UBlueprint* AsBlueprint = CastChecked<UBlueprint>(LocalAsset);
	check(LocalAsset->GetClass() == BaseAsset->GetClass());
	check(LocalAsset->GetClass() == RemoteAsset->GetClass());
	
	if (FAssetEditorManager::Get().OpenEditorForAsset(AsBlueprint))
	{
		FBlueprintEditorModule& BlueprintEditorModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>("Kismet");

		FBlueprintEditor* BlueprintEditor = static_cast<FBlueprintEditor*>(FAssetEditorManager::Get().FindEditorForAsset(AsBlueprint, /*bFocusIfOpen =*/false));
		BlueprintEditor->CreateMergeToolTab(Cast<UBlueprint>(BaseAsset), Cast<UBlueprint>(RemoteAsset), ResolutionCallback);
	}
}

bool FAssetTypeActions_Blueprint::CanCreateNewDerivedBlueprint() const
{
	return true;
}

void FAssetTypeActions_Blueprint::ExecuteEditDefaults(TArray<TWeakObjectPtr<UBlueprint>> Objects)
{
	TArray< UBlueprint* > Blueprints;

	FMessageLog EditorErrors("EditorErrors");
	EditorErrors.NewPage(LOCTEXT("ExecuteEditDefaultsNewLogPage", "Loading Blueprints"));

	for (auto ObjIt = Objects.CreateConstIterator(); ObjIt; ++ObjIt)
	{
		auto Object = (*ObjIt).Get();
		if ( Object )
		{
			// If the blueprint is valid, allow it to be added to the list, otherwise log the error.
			if (Object && Object->SkeletonGeneratedClass && Object->GeneratedClass )
			{
				Blueprints.Add(Object);
			}
			else
			{
				FFormatNamedArguments Arguments;
				Arguments.Add(TEXT("ObjectName"), FText::FromString(Object->GetName()));
				EditorErrors.Error(FText::Format(LOCTEXT("LoadBlueprint_FailedLog", "{ObjectName} could not be loaded because it derives from an invalid class.  Check to make sure the parent class for this blueprint hasn't been removed!"), Arguments ) );
			}
		}
	}

	if ( Blueprints.Num() > 0 )
	{
		FBlueprintEditorModule& BlueprintEditorModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>( "Kismet" );
		TSharedRef< IBlueprintEditor > NewBlueprintEditor = BlueprintEditorModule.CreateBlueprintEditor(  EToolkitMode::Standalone, TSharedPtr<IToolkitHost>(), Blueprints );
	}

	// Report errors
	EditorErrors.Notify(LOCTEXT("OpenDefaults_Failed", "Opening Class Defaults Failed!"));
}

void FAssetTypeActions_Blueprint::ExecuteNewDerivedBlueprint(TWeakObjectPtr<UBlueprint> InObject)
{
	auto Object = InObject.Get();
	if ( Object )
	{
		// The menu option should ONLY be available if there is only one blueprint selected, validated by the menu creation code
		UBlueprint* TargetParentBP = Object;
		UClass* TargetParentClass = TargetParentBP->GeneratedClass;

		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(TargetParentClass))
		{
			FMessageDialog::Open( EAppMsgType::Ok, LOCTEXT("InvalidClassToMakeBlueprintFrom", "Invalid class with which to make a Blueprint."));
			return;
		}

		FString Name;
		FString PackageName;
		CreateUniqueAssetName(Object->GetOutermost()->GetName(), TEXT("_Child"), PackageName, Name);

		UPackage* Package = CreatePackage(NULL, *PackageName);
		if (ensure(Package))
		{
			// Create and init a new Blueprint
			UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(TargetParentClass, Package, FName(*Name), BPTYPE_Normal, TargetParentBP->GetClass(), UBlueprintGeneratedClass::StaticClass());
			if (NewBP)
			{
				// Notify the asset registry
				FAssetRegistryModule::AssetCreated(NewBP);

				// the editor should be opened AFTER being added to the asset 
				// registry (some systems could queue off of the asset registry 
				// add event, and already having that blueprint open can be odd)
				FAssetEditorManager::Get().OpenEditorForAsset(NewBP);

				// Mark the package dirty...
				Package->MarkPackageDirty();
			}
		}
	}
}

FText FAssetTypeActions_Blueprint::GetNewDerivedBlueprintTooltip(TWeakObjectPtr<UBlueprint> InObject)
{
	if (!CanExecuteNewDerivedBlueprint(InObject))
	{
		return LOCTEXT("Blueprint_NewDerivedBlueprintIsDeprecatedTooltip", "Blueprint class is deprecated, cannot derive a child Blueprint!");
	}
	else
	{
		return LOCTEXT("Blueprint_NewDerivedBlueprintTooltip", "Creates a blueprint based on the selected blueprint.");
	}
}

bool FAssetTypeActions_Blueprint::CanExecuteNewDerivedBlueprint(TWeakObjectPtr<UBlueprint> InObject)
{
	auto BP = InObject.Get();
	auto BPGC = BP ? BP->GeneratedClass : NULL;
	return BPGC && !BPGC->HasAnyClassFlags(CLASS_Deprecated);
}

bool FAssetTypeActions_Blueprint::ShouldUseDataOnlyEditor( const UBlueprint* Blueprint ) const
{
	return FBlueprintEditorUtils::IsDataOnlyBlueprint(Blueprint) 
		&& !FBlueprintEditorUtils::IsLevelScriptBlueprint(Blueprint) 
		&& !FBlueprintEditorUtils::IsInterfaceBlueprint(Blueprint)
		&& !Blueprint->bForceFullEditor
		&& !Blueprint->bIsNewlyCreated;
}

void FAssetTypeActions_Blueprint::PerformAssetDiff(UObject* OldAsset, UObject* NewAsset, const FRevisionInfo& OldRevision, const FRevisionInfo& NewRevision) const
{
	UBlueprint* OldBlueprint = CastChecked<UBlueprint>(OldAsset);
	UBlueprint* NewBlueprint = CastChecked<UBlueprint>(NewAsset);

	// sometimes we're comparing different revisions of one single asset (other 
	// times we're comparing two completely separate assets altogether)
	bool bIsSingleAsset = (NewBlueprint->GetName() == OldBlueprint->GetName());

	FText WindowTitle = LOCTEXT("NamelessBlueprintDiff", "Blueprint Diff");
	// if we're diff'ing one asset against itself 
	if (bIsSingleAsset)
	{
		// identify the assumed single asset in the window's title
		WindowTitle = FText::Format(LOCTEXT("Blueprint Diff", "{0} - Blueprint Diff"), FText::FromString(NewBlueprint->GetName()));
	}

	const TSharedPtr<SWindow> Window = SNew(SWindow)
		.Title(WindowTitle)
		.ClientSize(FVector2D(1000,800));

	Window->SetContent(SNew(SBlueprintDiff)
					  .BlueprintOld(OldBlueprint)
					  .BlueprintNew(NewBlueprint)
					  .OldRevision(OldRevision)
					  .NewRevision(NewRevision)
					  .ShowAssetNames(!bIsSingleAsset) );

	// Make this window a child of the modal window if we've been spawned while one is active.
	TSharedPtr<SWindow> ActiveModal = FSlateApplication::Get().GetActiveModalWindow();
	if ( ActiveModal.IsValid() )
	{
		FSlateApplication::Get().AddWindowAsNativeChild( Window.ToSharedRef(), ActiveModal.ToSharedRef() );
	}
	else
	{
		FSlateApplication::Get().AddWindow( Window.ToSharedRef() );
	}
}

UThumbnailInfo* FAssetTypeActions_Blueprint::GetThumbnailInfo(UObject* Asset) const
{
	// Blueprint thumbnail scenes are disabled for now
	UBlueprint* Blueprint = CastChecked<UBlueprint>(Asset);
	UThumbnailInfo* ThumbnailInfo = Blueprint->ThumbnailInfo;
	if ( ThumbnailInfo == NULL )
	{
		ThumbnailInfo = ConstructObject<USceneThumbnailInfo>(USceneThumbnailInfo::StaticClass(), Blueprint);
		Blueprint->ThumbnailInfo = ThumbnailInfo;
	}

	return ThumbnailInfo;
}

FText FAssetTypeActions_Blueprint::GetAssetDescription(const FAssetData& AssetData) const
{
	if (const FString* pDescription = AssetData.TagsAndValues.Find(GET_MEMBER_NAME_CHECKED(UBlueprint, BlueprintDescription)))
	{
		if (!pDescription->IsEmpty())
		{
			const FString DescriptionStr(*pDescription);
			return FText::FromString(DescriptionStr.Replace(TEXT("\\n"), TEXT("\n")));
		}
	}

	return FText::GetEmpty();
}

TWeakPtr<IClassTypeActions> FAssetTypeActions_Blueprint::GetClassTypeActions(const FAssetData& AssetData) const
{
	static const FName NativeParentClassTag = "NativeParentClass";
	static const FName ParentClassTag = "ParentClass";

	// Blueprints get the class type actions for their parent native class - this avoids us having to load the blueprint
	UClass* ParentClass = nullptr;
	const FString* ParentClassNamePtr = AssetData.TagsAndValues.Find(NativeParentClassTag);
	if(!ParentClassNamePtr)
	{
		ParentClassNamePtr = AssetData.TagsAndValues.Find(ParentClassTag);
	}
	if(ParentClassNamePtr && !ParentClassNamePtr->IsEmpty())
	{
		UObject* Outer = nullptr;
		FString ParentClassName = *ParentClassNamePtr;
		ResolveName(Outer, ParentClassName, false, false);
		ParentClass = FindObject<UClass>(ANY_PACKAGE, *ParentClassName);
	}

	if(ParentClass)
	{
		FAssetToolsModule& AssetToolsModule = FAssetToolsModule::GetModule();
		return AssetToolsModule.Get().GetClassTypeActionsForClass(ParentClass);
	}

	return nullptr;
}

#undef LOCTEXT_NAMESPACE
