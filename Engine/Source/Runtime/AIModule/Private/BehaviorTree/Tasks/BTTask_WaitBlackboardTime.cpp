// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "AIModulePrivate.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Tasks/BTTask_WaitBlackboardTime.h"

UBTTask_WaitBlackboardTime::UBTTask_WaitBlackboardTime(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	NodeName = "Wait Blackboard Time";

	BlackboardKey.AddFloatFilter(this);
}

void UBTTask_WaitBlackboardTime::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);
	BlackboardKey.CacheSelectedKey(GetBlackboardAsset());
}

EBTNodeResult::Type UBTTask_WaitBlackboardTime::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	// Update wait time based on current blackboard key value
	const UBlackboardComponent* MyBlackboard = OwnerComp.GetBlackboardComponent();
	if (MyBlackboard && BlackboardKey.SelectedKeyType == UBlackboardKeyType_Float::StaticClass())
	{
		WaitTime = MyBlackboard->GetValue<UBlackboardKeyType_Float>(BlackboardKey.GetSelectedKeyID());
	}
	
	return Super::ExecuteTask(OwnerComp, NodeMemory);
}

FString UBTTask_WaitBlackboardTime::GetStaticDescription() const
{
	return FString::Printf(TEXT("%s: %s"), *UBTTaskNode::GetStaticDescription(), *GetSelectedBlackboardKey().ToString());
}

