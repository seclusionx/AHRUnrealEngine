// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "UMGPrivatePCH.h"

#define LOCTEXT_NAMESPACE "UMG"

/////////////////////////////////////////////////////
// UComboBoxString

UComboBoxString::UComboBoxString(const FPostConstructInitializeProperties& PCIP)
	: Super(PCIP)
{
	ContentPadding = FMargin(4.0, 2.0);
	MaxListHeight = 450.0f;
	HasDownArrow = true;
}

void UComboBoxString::ReleaseNativeWidget()
{
	Super::ReleaseNativeWidget();

	MyComboBox.Reset();
	ComoboBoxContent.Reset();
}

TSharedRef<SWidget> UComboBoxString::RebuildWidget()
{
	for ( FString& DefaultOptions : DefaultOptions )
	{
		AddOption(DefaultOptions);
	}

	TSharedPtr<FString> InitiallySelectedItem;
	int32 InitialIndex = FindOptionIndex(InitiallySelectedOption);
	if ( InitialIndex != -1 )
	{
		InitiallySelectedItem = Options[InitialIndex];
	}

	MyComboBox =
		SNew(SComboBox< TSharedPtr<FString> >)
		.OptionsSource(&Options)
		.InitiallySelectedItem(InitiallySelectedItem)
		.ContentPadding(ContentPadding)
		.MaxListHeight(MaxListHeight)
		.HasDownArrow(HasDownArrow)
		.Method(SMenuAnchor::UseCurrentWindow)
		.OnGenerateWidget(BIND_UOBJECT_DELEGATE(SComboBox< TSharedPtr<FString> >::FOnGenerateWidget, HandleGenerateWidget))
		.OnSelectionChanged(BIND_UOBJECT_DELEGATE(SComboBox< TSharedPtr<FString> >::FOnSelectionChanged, HandleSelectionChanged))
		.OnComboBoxOpening(BIND_UOBJECT_DELEGATE(FOnComboBoxOpening, HandleOpening))
		[
			SAssignNew(ComoboBoxContent, SBox)
		];

	if ( InitialIndex != -1 )
	{
		// Generate the widget for the initially selected widget if needed
		ComoboBoxContent->SetContent(HandleGenerateWidget(InitiallySelectedItem));
	}

	//TODO UMG Expose the rest of these options

	//SLATE_STYLE_ARGUMENT(FComboBoxStyle, ComboBoxStyle)

	//	/** The visual style of the button part of the combo box (overrides ComboBoxStyle) */
	//	SLATE_STYLE_ARGUMENT(FButtonStyle, ButtonStyle)

	//	/** The sound to play when the button is pressed (overrides ComboBoxStyle) */
	//	SLATE_ARGUMENT(TOptional<FSlateSound>, PressedSoundOverride)

	//	/** The sound to play when the selection changes (overrides ComboBoxStyle) */
	//	SLATE_ARGUMENT(TOptional<FSlateSound>, SelectionChangeSoundOverride)

	return MyComboBox.ToSharedRef();
}

void UComboBoxString::AddOption(const FString& Option)
{
	Options.Add(MakeShareable(new FString(Option)));
}

bool UComboBoxString::RemoveOption(const FString& Option)
{
	int32 OptionIndex = FindOptionIndex(Option);
	if ( OptionIndex != -1 )
	{
		Options.RemoveAt(OptionIndex);
		return true;
	}

	return false;
}

int32 UComboBoxString::FindOptionIndex(const FString& Option)
{
	for ( int32 OptionIndex = 0; OptionIndex < Options.Num(); OptionIndex++ )
	{
		TSharedPtr<FString>& OptionAtIndex = Options[OptionIndex];

		if ( ( *OptionAtIndex ) == Option )
		{
			return OptionIndex;
		}
	}

	return -1;
}

void UComboBoxString::ClearOptions()
{
	Options.Empty();
}

TSharedRef<SWidget> UComboBoxString::HandleGenerateWidget(TSharedPtr<FString> Item) const
{
	// Call the user's delegate to see if they want to generate a custom widget bound to the data source.
	if ( !IsDesignTime() && OnGenerateWidgetEvent.IsBound() )
	{
		UWidget* Widget = OnGenerateWidgetEvent.Execute(*Item);
		if ( Widget != NULL )
		{
			return Widget->TakeWidget();
		}
	}

	// If a row wasn't generated just create the default one, a simple text block of the item's name.
	return SNew(STextBlock).Text(FText::FromString(*Item));
}

void UComboBoxString::HandleSelectionChanged(TSharedPtr<FString> Item, ESelectInfo::Type SelectionType)
{
	if ( !IsDesignTime() )
	{
		OnSelectionChanged.Broadcast(*Item, SelectionType);
	}

	// When the selection changes we always generate another widget to represent the content area of the comobox.
	ComoboBoxContent->SetContent( HandleGenerateWidget(Item) );
}

void UComboBoxString::HandleOpening()
{
	OnOpening.Broadcast();
}

#if WITH_EDITOR

const FSlateBrush* UComboBoxString::GetEditorIcon()
{
	return FUMGStyle::Get().GetBrush("Widget.ComboBox");
}

const FText UComboBoxString::GetToolboxCategory()
{
	return LOCTEXT("Misc", "Misc");
}

#endif

/////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE