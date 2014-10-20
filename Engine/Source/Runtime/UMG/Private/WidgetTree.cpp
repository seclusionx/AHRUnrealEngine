// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "UMGPrivatePCH.h"

/////////////////////////////////////////////////////
// UWidgetTree

UWidgetTree::UWidgetTree(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UWidget* UWidgetTree::FindWidget(const FName& Name) const
{
	UWidget* FoundWidget = nullptr;

	ForEachWidget([&] (UWidget* Widget) {
		if ( Widget->GetFName() == Name )
		{
			FoundWidget = Widget;
		}
	});

	return FoundWidget;
}

UWidget* UWidgetTree::FindWidget(TSharedRef<SWidget> InWidget) const
{
	UWidget* FoundWidget = nullptr;

	ForEachWidget([&] (UWidget* Widget) {
		if ( Widget->GetCachedWidget() == InWidget )
		{
			FoundWidget = Widget;
		}
	});

	return FoundWidget;
}

UPanelWidget* UWidgetTree::FindWidgetParent(UWidget* Widget, int32& OutChildIndex)
{
	UPanelWidget* Parent = Widget->GetParent();
	if ( Parent != nullptr )
	{
		OutChildIndex = Parent->GetChildIndex(Widget);
	}
	else
	{
		OutChildIndex = 0;
	}

	return Parent;
}

bool UWidgetTree::RemoveWidget(UWidget* InRemovedWidget)
{
	bool bRemoved = false;

	UPanelWidget* InRemovedWidgetParent = InRemovedWidget->GetParent();
	if ( InRemovedWidgetParent )
	{
		if ( InRemovedWidgetParent->RemoveChild(InRemovedWidget) )
		{
			bRemoved = true;
		}
	}
	// If the widget being removed is the root, null it out.
	else if ( InRemovedWidget == RootWidget )
	{
		RootWidget = NULL;
		bRemoved = true;
	}

	return bRemoved;
}

void UWidgetTree::GetAllWidgets(TArray<UWidget*>& Widgets) const
{
	ForEachWidget([&] (UWidget* Widget) { Widgets.Add(Widget); });
}

void UWidgetTree::GetChildWidgets(UWidget* Parent, TArray<UWidget*>& Widgets) const
{
	ForWidgetAndChildren(Parent, [&] (UWidget* Widget) { Widgets.Add(Widget); });
}
