#include "Nodes/World/FlowNode_PlayLevelSequence.h"

#include "FlowModule.h"
#include "FlowSubsystem.h"
#include "MovieScene/MovieSceneFlowTrack.h"
#include "MovieScene/MovieSceneFlowTriggerSection.h"

#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "VisualLogger/VisualLogger.h"

FFlowNodeLevelSequenceEvent UFlowNode_PlayLevelSequence::OnPlaybackStarted;
FFlowNodeLevelSequenceEvent UFlowNode_PlayLevelSequence::OnPlaybackCompleted;

UFlowNode_PlayLevelSequence::UFlowNode_PlayLevelSequence(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, LoadedSequence(nullptr)
	, SequencePlayer(nullptr)
{
#if WITH_EDITOR
	Category = TEXT("World");
	NodeStyle = EFlowNodeStyle::Latent;
#endif

	OutputNames.Add(TEXT("PreStart"));
	OutputNames.Add(TEXT("Started"));
	OutputNames.Add(TEXT("Completed"));
}

#if WITH_EDITOR
TArray<FName> UFlowNode_PlayLevelSequence::GetContextOutputs()
{
	if (Sequence.IsNull())
	{
		return TArray<FName>();
	}

	TArray<FName> PinNames = {};

	Sequence.LoadSynchronous();
	if (Sequence && Sequence->GetMovieScene())
	{
		for (const UMovieSceneTrack* Track : Sequence->GetMovieScene()->GetMasterTracks())
		{
			if (Track->GetClass() == UMovieSceneFlowTrack::StaticClass())
			{
				for (UMovieSceneSection* Section : Track->GetAllSections())
				{
					if (UMovieSceneFlowSectionBase* FlowSection = Cast<UMovieSceneFlowSectionBase>(Section))
					{
						for (const FString& EventName : FlowSection->GetAllEntryPoints())
						{
							if (!EventName.IsEmpty())
							{
								PinNames.Emplace(EventName);
							}
						}
					}
				}
			}
		}
	}

	return PinNames;
}
#endif

void UFlowNode_PlayLevelSequence::PreloadContent()
{
#if ENABLE_VISUAL_LOG
	UE_VLOG(this, LogFlow, Log, TEXT("Preloading"));
#endif

	if (!Sequence.IsNull())
	{
		StreamableManager.RequestAsyncLoad({Sequence.ToSoftObjectPath()}, FStreamableDelegate());
	}
}

void UFlowNode_PlayLevelSequence::FlushContent()
{
#if ENABLE_VISUAL_LOG
	UE_VLOG(this, LogFlow, Log, TEXT("Flushing preload"));
#endif

	if (!Sequence.IsNull())
	{
		StreamableManager.Unload(Sequence.ToSoftObjectPath());
	}
}

void UFlowNode_PlayLevelSequence::CreatePlayer(const FMovieSceneSequencePlaybackSettings& PlaybackSettings)
{
	LoadedSequence = LoadAsset<ULevelSequence>(Sequence);
	if (LoadedSequence)
	{
		ALevelSequenceActor* SequenceActor;
		SequencePlayer = ULevelSequencePlayer::CreateLevelSequencePlayer(this, LoadedSequence, PlaybackSettings, SequenceActor);

		// todo: see https://github.com/MothCocoon/Flow/issues/9
		//SequenceActor->AdditionalEventReceivers = {this}; 

		// bind Flow Track events
		for (const UMovieSceneTrack* Track : LoadedSequence->GetMovieScene()->GetMasterTracks())
		{
			if (Track->GetClass() == UMovieSceneFlowTrack::StaticClass())
			{
				for (UMovieSceneSection* Section : Track->GetAllSections())
				{
					if (UMovieSceneFlowSectionBase* FlowSection = Cast<UMovieSceneFlowSectionBase>(Section))
					{
						// section evaluation is compiled once, it won't get pointer to node's instance
						FlowSection->OnEventExecuted.BindStatic(&UFlowNode_PlayLevelSequence::OnSequenceEventExecuted);
					}
				}
			}
		}
	}
}

void UFlowNode_PlayLevelSequence::ExecuteInput(const FName& PinName)
{
	const UWorld* World = GetFlowSubsystem()->GetWorld();
	LoadedSequence = LoadAsset<ULevelSequence>(Sequence);

	if (World && LoadedSequence)
	{
		ALevelSequenceActor* SequenceActor;
		SequencePlayer = ULevelSequencePlayer::CreateLevelSequencePlayer(this, LoadedSequence, FMovieSceneSequencePlaybackSettings(), SequenceActor);

		TriggerOutput(TEXT("PreStart"));

		SequencePlayer->OnFinished.AddDynamic(this, &UFlowNode_PlayLevelSequence::OnPlaybackFinished);
		SequencePlayer->Play();
		
		TriggerOutput(TEXT("Started"));
	}

	TriggerFirstOutput(false);
}

void UFlowNode_PlayLevelSequence::OnSequenceEventExecuted(UObject* EventReceiver, const FString& EventName)
{
	if (UFlowNode_PlayLevelSequence* Node = Cast<UFlowNode_PlayLevelSequence>(EventReceiver))
	{
		Node->TriggerEvent(EventName);
	}
}

void UFlowNode_PlayLevelSequence::TriggerEvent(const FString& EventName)
{
	TriggerOutput(*EventName, false);
}

void UFlowNode_PlayLevelSequence::OnTimeDilationUpdate(const float NewTimeDilation) const
{
	if (SequencePlayer)
	{
		SequencePlayer->SetPlayRate(NewTimeDilation);
	}
}

void UFlowNode_PlayLevelSequence::OnPlaybackFinished()
{
	TriggerOutput(TEXT("Completed"));
}

void UFlowNode_PlayLevelSequence::Cleanup()
{
	if (LoadedSequence)
	{
		for (const UMovieSceneTrack* Track : LoadedSequence->GetMovieScene()->GetMasterTracks())
		{
			if (Track->GetClass() == UMovieSceneFlowTrack::StaticClass())
			{
				for (UMovieSceneSection* Section : Track->GetAllSections())
				{
					if (UMovieSceneFlowSectionBase* FlowSection = Cast<UMovieSceneFlowSectionBase>(Section))
					{
						FlowSection->OnEventExecuted.Unbind();
					}
				}
			}
		}

		LoadedSequence = nullptr;
	}

	if (SequencePlayer)
	{
		SequencePlayer->OnFinished.RemoveAll(this);
		SequencePlayer->Stop();
		SequencePlayer = nullptr;
	}

#if ENABLE_VISUAL_LOG
	UE_VLOG(this, LogFlow, Log, TEXT("Finished playback: %s"), *Sequence.ToString());
#endif
}

FString UFlowNode_PlayLevelSequence::GetPlaybackProgress() const
{
	if (SequencePlayer && SequencePlayer->IsPlaying())
	{
		return GetProgressAsString(SequencePlayer->GetCurrentTime().AsSeconds()) + TEXT(" / ") + GetProgressAsString(SequencePlayer->GetDuration().AsSeconds());
	}

	return FString();
}

#if WITH_EDITOR
FString UFlowNode_PlayLevelSequence::GetNodeDescription() const
{
	return Sequence.IsNull() ? TEXT("[No sequence]") : Sequence.GetAssetName();
}

FString UFlowNode_PlayLevelSequence::GetStatusString() const
{
	return GetPlaybackProgress();
}

UObject* UFlowNode_PlayLevelSequence::GetAssetToOpen()
{
	return Sequence.IsNull() ? nullptr : LoadAsset<UObject>(Sequence);
}
#endif

#if ENABLE_VISUAL_LOG
void UFlowNode_PlayLevelSequence::GrabDebugSnapshot(struct FVisualLogEntry* Snapshot) const
{
	FVisualLogStatusCategory NewCategory = FVisualLogStatusCategory(TEXT("Sequence"));
	NewCategory.Add(*Sequence.ToString(), FString());
	Snapshot->Status.Add(NewCategory);
}
#endif