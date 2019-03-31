#ifndef polar_source_cpp
#define polar_source_cpp


POLAR_SOURCE_SOLO polar_source_CopyFromContainer(POLAR_SOURCE *Sources, u32 Index)
{
    POLAR_SOURCE_SOLO Result = {};

    memcpy(Result.Name, Sources->Name[Index], sizeof(Result.Name));
    Result.UID                      = Sources->UID[Index];
    Result.Type                     = Sources->Type[Index];
    Result.PlayState                = Sources->PlayState[Index];
    Result.States                   = Sources->States[Index];
    Result.FX                       = Sources->FX[Index];
    Result.Channels                 = Sources->Channels[Index];
    Result.SampleRate               = Sources->SampleRate[Index];
    Result.SampleCount              = Sources->SampleCount[Index];
    Result.BufferSize               = Sources->BufferSize[Index];
    Result.Buffer                   = Sources->Buffer[Index];

    return Result;
}

void polar_source_CopyToContainer(POLAR_SOURCE_SOLO Source, POLAR_SOURCE *Sources, u32 Index)
{
    memcpy(Sources->Name[Index], Source.Name, sizeof(Sources->Name[Index]));
    Sources->UID[Index]             = Source.UID;
    Sources->Type[Index]            = Source.Type;
    Sources->PlayState[Index]       = Source.PlayState;
    Sources->States[Index]          = Source.States;
    Sources->FX[Index]              = Source.FX;
    Sources->Channels[Index]        = Source.Channels;
    Sources->SampleRate[Index]      = Source.SampleRate;
    Sources->SampleCount[Index]     = Source.SampleCount;
    Sources->BufferSize[Index]      = Source.BufferSize;
    Sources->Buffer[Index]          = Source.Buffer;
}

POLAR_SOURCE *polar_source_Retrieval(POLAR_MIXER *Mixer, u64 UID, u32 &SourceIndex)
{
    for(POLAR_SUBMIX *SubmixIndex = Mixer->FirstInList; SubmixIndex; SubmixIndex = SubmixIndex->NextSubmix)
    {
        for(u32 i = 0; i <= SubmixIndex->Containers.CurrentContainers; ++i)
        {
            //Looking for blank source in a specifc container
            if(SubmixIndex->Containers.UID[i] == UID)
            {
                for(u32 j = 0; j <= SubmixIndex->Containers.Sources[i].CurrentSources; ++j)
                {
                    if(SubmixIndex->Containers.Sources[i].UID[j] == 0)
                    {
                        SourceIndex = j;
                        return &SubmixIndex->Containers.Sources[i];
                    }
                }
            }

            //Looking for specific source in all containers
            else
            {
                for(u32 j = 0; j <= SubmixIndex->Containers.Sources[i].CurrentSources; ++j)
                {
                    if(SubmixIndex->Containers.Sources[i].UID[j] == UID)
                    {
                        SourceIndex = j;
                        return &SubmixIndex->Containers.Sources[i];
                    }
                }
            }
        }

        //Same as above but for child submixes
        for(POLAR_SUBMIX *ChildSubmixIndex = SubmixIndex->ChildSubmix; ChildSubmixIndex; ChildSubmixIndex = ChildSubmixIndex->ChildSubmix)
        {
            for(u32 i = 0; i <= ChildSubmixIndex->Containers.CurrentContainers; ++i)
            {
                if(ChildSubmixIndex->Containers.UID[i] == UID)
                {
                    for(u32 j = 0; j <= ChildSubmixIndex->Containers.Sources[i].CurrentSources; ++j)
                    {
                        if(ChildSubmixIndex->Containers.Sources[i].UID[j] == 0)
                        {
                            SourceIndex = j;
                            return &ChildSubmixIndex->Containers.Sources[i];
                        }
                    }
                }

                else
                {
                    for(u32 j = 0; j <= ChildSubmixIndex->Containers.Sources[i].CurrentSources; ++j)
                    {
                        if(ChildSubmixIndex->Containers.Sources[i].UID[j] == UID)
                        {
                            SourceIndex = j;
                            return &ChildSubmixIndex->Containers.Sources[i];
                        }
                    }
                }
            }
        }
    }

    polar_Log("ERROR: Failed to retrieve %llu\n", UID);
    
    return 0;
}


void polar_source_Create(MEMORY_ARENA *Arena, POLAR_MIXER *Mixer, POLAR_ENGINE Engine, const char ContainerUID[MAX_STRING_LENGTH], const char SourceUID[MAX_STRING_LENGTH], u32 Channels, u32 Type, ...)
{
    u32 i = 0;
    POLAR_SOURCE *Sources = polar_source_Retrieval(Mixer, Hash(ContainerUID), i);
    
    if(!Sources)
    {
        polar_Log("ERROR: Failed to add source %s to container %s\n", SourceUID, ContainerUID);
        return;
    }
    
    POLAR_SOURCE_SOLO Source = polar_source_CopyFromContainer(Sources, i);

    if(!Source.UID)
    {
        memcpy(Source.Name, SourceUID, sizeof(Source.Name));
        Source.UID = Hash(SourceUID);
        Source.PlayState = Stopped;
        Source.Channels = Channels;

        Source.States.CurrentEnvelopes = 0;

        Source.States.Amplitude.Current = 0;
        Source.States.Amplitude.Previous = Source.States.Amplitude.Current;

        Source.States.Position = {};

        Source.SampleRate = Engine.SampleRate;
        Source.BufferSize = Engine.BufferSize;
        Source.SampleCount = 0;
        Source.States.PanPositions = (f32 *) memory_arena_Push(Arena, Source.States.PanPositions, Source.Channels);
        Source.Buffer = (f32 *) memory_arena_Push(Arena, Source.Buffer, Source.BufferSize);

        bool SourceCreated = false;
        Source.Type.Flag = Type;

        switch(Source.Type.Flag)
        {
            case SO_FILE:
            {
                va_list ArgList;
                va_start(ArgList, Type);
                char *Name = va_arg(ArgList, char *);

                char FilePath[MAX_STRING_LENGTH];
                polar_StringConcatenate(StringLength(AssetPath), AssetPath, StringLength(Name), Name, FilePath);

                Source.Type.File = (POLAR_FILE *) memory_arena_Push(Arena, Source.Type.File, (sizeof (POLAR_FILE)));
                Source.Type.File->Samples = drwav_open_file_and_read_pcm_frames_f32(FilePath, &Source.Type.File->Channels, &Source.Type.File->SampleRate, &Source.Type.File->FrameCount);  

                if(!Source.Type.File->Samples)
                {
                    polar_Log("ERROR: Cannot open file %s\n", FilePath);
                    break;
                }

                if(Source.Type.File->SampleRate != Engine.SampleRate)
                {
                    f32 *ResampleBuffer = 0;
                    ResampleBuffer = (f32 *) memory_arena_Push(Arena, ResampleBuffer, (sizeof (f32) * Source.Type.File->FrameCount));

                    switch(Engine.SampleRate)
                    {
                        case 48000:
                        {
                            switch(Source.Type.File->SampleRate)
                            {
                                case 44100:
                                {
                                    Resample(Source.Type.File->Samples, Source.Type.File->FrameCount, ResampleBuffer, (Source.Type.File->FrameCount) * 0.91875f);
                                }
                            }
                        }

                        case 192000:
                        {
                            switch(Source.Type.File->SampleRate)
                            {
                                case 44100:
                                {
                                    // Resample(Source.Type.File->Samples, Source.Type.File->FrameCount, ResampleBuffer, (Source.Type.File->FrameCount) * 0.435374f);
                                    // Resample(Source.Type.File->Samples, Source.Type.File->FrameCount, ResampleBuffer, (Source.Type.File->FrameCount) * 1.0f);
                                }
                            }
                        }
                    }

                    Source.Type.File->Samples = ResampleBuffer;
                }

                va_end(ArgList);

                SourceCreated = true;
                break;
            }

            case SO_OSCILLATOR:
            {
                va_list ArgList;
                va_start(ArgList, Type);
                u32 Wave = va_arg(ArgList, u32);
                f32 Frequency = va_arg(ArgList, f64);
                Source.Type.Oscillator = polar_dsp_OscillatorCreate(Arena, Engine.SampleRate, Wave, Frequency);

                va_end(ArgList);

                SourceCreated = true;
                break;
            }

            default:
            {
                Source.Type.Oscillator = 0;
                Source.Type.File = 0;
                break;
            }
        }

        if(SourceCreated)
        {
            Source.FX = FX_DRY;
            Sources->CurrentSources += 1;
        }
    }

    else
    {
        polar_Log("ERROR: Failed to add source %s to container %s\n", SourceUID, ContainerUID);
        return;
    }

    polar_source_CopyToContainer(Source, Sources, i);    
    
    return;
}

void polar_source_CreateFromFile(MEMORY_ARENA *Arena, POLAR_MIXER *Mixer, POLAR_ENGINE Engine, const char *FileName)
{
    FILE *File;

    char FilePath[MAX_STRING_LENGTH];
    polar_StringConcatenate(StringLength(AssetPath), AssetPath, StringLength(FileName), FileName, FilePath);

#if _WIN32    
    fopen_s(&File, FilePath, "r");
#else
    File = fopen(FilePath, "r");
#endif

    Assert(File);

    char SourceLine[256];
    i32 FileError = 0;
    u64 Index = 0;
    
    char Container[128];
    char Source[128];
    u32 Channels = 0;
    char Type[32];
    f64 Frequency = 0;

    while(fgets(SourceLine, 256, File))
    {
        FileError = sscanf(SourceLine, "%s %s %d %s %lf", Container, Source, &Channels, Type, &Frequency);
        if(strncmp(Type, "SO_OSCILLATOR", 32) == 0)
        {
            polar_source_Create(Arena, Mixer, Engine, Container, Source, Channels, SO_OSCILLATOR, WV_SINE, Frequency);
        }
        ++Index;
    }
}


void polar_source_UpdateFadeState(f64 GlobalTime, POLAR_STATE_FADE *State)
{
    if(State->Current == State->EndValue)
    {
        return;
    }

    f32 TimePassed = (GlobalTime - State->StartTime);
    f32 FadeCompletion = (TimePassed / State->Duration);
    State->Current = (((State->EndValue - State->StartValue) * FadeCompletion) + State->StartValue);

    if(FadeCompletion >= 1.0f)
    {
        State->Current = State->EndValue;
        State->Duration = 0.0f;
        State->IsFading = false;
    }
}

void polar_source_UpdatePerSampleState(f64 UpdatePeriod, POLAR_STATE_PER_SAMPLE *State)
{
    if(UpdatePeriod <= 0.0f)
    {
        State->Current = State->Target;
    }

    else
    {
        f64 OneOverFade = 1.0f / UpdatePeriod;
        State->Delta = (OneOverFade * (State->Target - State->Current));
    }
}


void polar_source_Fade(POLAR_MIXER *Mixer, u64 SourceUID, f64 GlobalTime, f32 NewAmplitude, f32 Duration)
{
    u32 i = 0;
    POLAR_SOURCE *Sources = polar_source_Retrieval(Mixer, SourceUID, i);
    if(!Sources)
    {
        polar_Log("ERROR: Cannot fade source %llu\n", SourceUID);
        return;
    }

    POLAR_SOURCE_SOLO Source = polar_source_CopyFromContainer(Sources, i);

    if(Source.UID)
    {
        Source.States.Amplitude.StartValue = Source.States.Amplitude.Current;
        Source.States.Amplitude.EndValue = NewAmplitude;
        Source.States.Amplitude.Duration = MAX(Duration, 0.0f);
        Source.States.Amplitude.StartTime = GlobalTime;
        Source.States.Amplitude.IsFading = true;
    }

    else
    {
        polar_Log("ERROR: Cannot fade source %llu\n", SourceUID);
        return;
    }


    polar_source_CopyToContainer(Source, Sources, i);

    return; 
}

void polar_source_Attenuation(POLAR_MIXER *Mixer, u64 SourceUID, bool IsAttenuated, f32 MinDistance, f32 MaxDistance, f32 Rolloff)
{
    u32 i = 0;
    POLAR_SOURCE *Sources = polar_source_Retrieval(Mixer, SourceUID, i);
    if(!Sources)
    {
        polar_Log("ERROR: Cannot edit attenuation on source %llu\n", SourceUID);
        return;
    }

    POLAR_SOURCE_SOLO Source = polar_source_CopyFromContainer(Sources, i);

    if(Source.UID)
    {
        if(IsAttenuated)
        {
            Source.States.IsDistanceAttenuated = true;

            f32 OldMinDistance = Source.States.MinDistance;
            f32 OldMaxDistance = Source.States.MaxDistance;
            f32 OldRolloff = Source.States.Rolloff;

            if(OldMinDistance != MinDistance || OldMaxDistance != MaxDistance || OldRolloff != Rolloff)
            {
                Source.States.RolloffDirty = true;
            }

            Source.States.MinDistance = MinDistance;
            Source.States.MaxDistance = MaxDistance;
            Source.States.Rolloff = Rolloff;
        }

        else
        {
            Source.States.IsDistanceAttenuated = false;
        }
    }

    else
    {
        polar_Log("ERROR: Cannot edit attenuation on source %llu\n", SourceUID);
        return;
    }

    polar_source_CopyToContainer(Source, Sources, i);

    return; 
}



void polar_source_Update(POLAR_MIXER *Mixer, POLAR_SOURCE *Sources, u32 &SourceIndex, f64 GlobalTime, f64 MinPeriod, f32 NoiseFloor)
{
    bool IsEnvelope = false;

    //Update envelopes if they exist
    for(u8 EnvelopeIndex = 0; EnvelopeIndex < Sources->States[SourceIndex].CurrentEnvelopes; ++EnvelopeIndex)
    {
        if(Sources->States[SourceIndex].Envelope[EnvelopeIndex].CurrentPoints > 0 && Sources->States[SourceIndex].Envelope[EnvelopeIndex].Index <= Sources->States[SourceIndex].Envelope[EnvelopeIndex].CurrentPoints)
        {
            IsEnvelope = true;

            switch(Sources->States[SourceIndex].Envelope[EnvelopeIndex].Assignment)
            {
                case EN_AMPLITUDE:
                {
                    u32 PointIndex = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Index;
                    f32 Current = Sources->States[SourceIndex].Amplitude.Current;

                    if(PointIndex == 0)
                    {
                        f64 Amplitude = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex].Value;
                        f64 Duration = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex + 1].Time - Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex].Time;
                        
                        if(Duration < MinPeriod)
                        {
                            Duration = MinPeriod;
                        }

                        Sources->States[SourceIndex].Amplitude.StartValue = Sources->States[SourceIndex].Amplitude.Current;
                        Sources->States[SourceIndex].Amplitude.EndValue = Amplitude;
                        Sources->States[SourceIndex].Amplitude.Duration = MAX(Duration, 0.0f);
                        Sources->States[SourceIndex].Amplitude.StartTime = GlobalTime;
                        Sources->States[SourceIndex].Amplitude.IsFading = true;

                        ++Sources->States[SourceIndex].Envelope[EnvelopeIndex].Index;
                        break;
                    }

                    if(Current == Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex - 1].Value)
                    {
                        f64 Amplitude = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex].Value;
                        f64 Duration = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex + 1].Time - Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex].Time;
                        
                        if(Duration < MinPeriod)
                        {
                            Duration = MinPeriod;
                        }

                        Sources->States[SourceIndex].Amplitude.StartValue = Sources->States[SourceIndex].Amplitude.Current;
                        Sources->States[SourceIndex].Amplitude.EndValue = Amplitude;
                        Sources->States[SourceIndex].Amplitude.Duration = MAX(Duration, 0.0f);
                        Sources->States[SourceIndex].Amplitude.StartTime = GlobalTime;
                        Sources->States[SourceIndex].Amplitude.IsFading = true;

                        ++Sources->States[SourceIndex].Envelope[EnvelopeIndex].Index;
                        break;
                    }

                    if((PointIndex + 1) == (Sources->States[SourceIndex].Envelope[EnvelopeIndex].CurrentPoints - 1))
                    {
                        f64 Amplitude = 0.0f;
                        f64 Duration = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex + 1].Time - Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex].Time;
                        
                        if(Duration < MinPeriod)
                        {
                            Duration = MinPeriod;
                        }


                        Sources->States[SourceIndex].Amplitude.StartValue = Sources->States[SourceIndex].Amplitude.Current;
                        Sources->States[SourceIndex].Amplitude.EndValue = Amplitude;
                        Sources->States[SourceIndex].Amplitude.Duration = MAX(Duration, 0.0f);
                        Sources->States[SourceIndex].Amplitude.StartTime = GlobalTime;
                        Sources->States[SourceIndex].Amplitude.IsFading = true;

                        ++Sources->States[SourceIndex].Envelope[EnvelopeIndex].Index;
                        break;                        
                    }

                    break;
                }

                case EN_FREQUENCY:
                {
                    u32 PointIndex = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Index;
                    f32 Start = Sources->Type[SourceIndex].Oscillator->Frequency.Current;
                    f32 End = Sources->Type[SourceIndex].Oscillator->Frequency.Target;

                    f32 RangeTolerance = 1.0f;

                    if(PointIndex == 0)
                    {                    
                        f64 Frequency = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex].Value;
                        f64 Duration = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex + 1].Time - Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex].Time;

                        if(Duration < MinPeriod)
                        {
                            Duration = MinPeriod;
                        }

                        Sources->Type[SourceIndex].Oscillator->Frequency.Target = Frequency;
                        polar_source_UpdatePerSampleState(Duration, &Sources->Type[SourceIndex].Oscillator->Frequency);

                        ++PointIndex;
                        ++Sources->States[SourceIndex].Envelope[EnvelopeIndex].Index;
                    }

                    else
                    {
                        if(CheckFloat(Start, (End - RangeTolerance), (End + RangeTolerance)))
                        {
                            f64 Frequency = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex].Value;
                            f64 Duration = Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex + 1].Time - Sources->States[SourceIndex].Envelope[EnvelopeIndex].Points[PointIndex].Time;

                            if(Duration < MinPeriod)
                            {
                                Duration = MinPeriod;
                            }

                            Sources->Type[SourceIndex].Oscillator->Frequency.Target = Frequency;
                            polar_source_UpdatePerSampleState(Duration, &Sources->Type[SourceIndex].Oscillator->Frequency);

                            ++PointIndex;
                            ++Sources->States[SourceIndex].Envelope[EnvelopeIndex].Index;
                        }
                    }
                    
                    break;
                }
                
                default:
                {
                    break;
                }
            }
        }
    }


    //Update distance attenuation
    if(Sources->States[SourceIndex].IsDistanceAttenuated)
    {
        polar_listener_DistanceFromListener(Mixer->Listener, Sources->States[SourceIndex], NoiseFloor);

        f32 DistanceAmp = (Sources->States[SourceIndex].Amplitude.Current - (Sources->States[SourceIndex].Amplitude.Current * Sources->States[SourceIndex].DistanceAttenuation));
        Sources->States[SourceIndex].DistanceAttenuation = DistanceAmp;
    }
    
    //Update per-sample parameters
    if(Sources->States[SourceIndex].Amplitude.IsFading)
    {
        polar_source_UpdateFadeState(GlobalTime, &Sources->States[SourceIndex].Amplitude);
    }
    
    Sources->States[SourceIndex].Pan = (Mixer->Listener->Rotation.Yaw / Sources->States[SourceIndex].DistanceFromListener);
    Sources->States[SourceIndex].Pan = -Sources->States[SourceIndex].Pan;

    // polar_Log("Current: %f Previous: %f\n", Sources->States[SourceIndex].Amplitude.Current, Sources->States[SourceIndex].Amplitude.Previous);
}


void polar_source_UpdatePlaying(POLAR_MIXER *Mixer, f64 GlobalTime, f64 MinPeriod, f32 NoiseFloor)
{
    for(POLAR_SUBMIX *SubmixIndex = Mixer->FirstInList; SubmixIndex; SubmixIndex = SubmixIndex->NextSubmix)
    {
        for(u32 i = 0; i <= SubmixIndex->Containers.CurrentContainers; ++i)
        {
            for(u32 j = 0; j <= SubmixIndex->Containers.Sources[i].CurrentSources; ++j)
            {
                if(SubmixIndex->Containers.Sources[i].PlayState[j] == Playing)
                {
                    POLAR_SOURCE *Sources = &SubmixIndex->Containers.Sources[i];
                    polar_source_Update(Mixer, Sources, j, GlobalTime, MinPeriod, NoiseFloor);
                }
            }
        }

        for(POLAR_SUBMIX *ChildSubmixIndex = SubmixIndex->ChildSubmix; ChildSubmixIndex; ChildSubmixIndex = ChildSubmixIndex->ChildSubmix)
        {
            for(u32 i = 0; i <= ChildSubmixIndex->Containers.CurrentContainers; ++i)
            {
                for(u32 j = 0; j <= ChildSubmixIndex->Containers.Sources[i].CurrentSources; ++j)
                {
                    if(ChildSubmixIndex->Containers.Sources[i].PlayState[j] == Playing)
                    {
                        POLAR_SOURCE *Sources = &ChildSubmixIndex->Containers.Sources[i];
                        polar_source_Update(Mixer, Sources, j, GlobalTime, MinPeriod, NoiseFloor);
                    }
                }
            }
        }
    }
}


void polar_source_Position(POLAR_MIXER *Mixer, u64 SourceUID, VECTOR4D NewPosition)
{
    u32 i = 0;
    POLAR_SOURCE *Sources = polar_source_Retrieval(Mixer, SourceUID, i);
    if(!Sources)
    {
        polar_Log("ERROR: Cannot update position of source %llu\n", SourceUID);
        return;
    }

    POLAR_SOURCE_SOLO Source = polar_source_CopyFromContainer(Sources, i);

    if(Source.UID)
    {
        Source.States.Position = NewPosition;
    }

    else
    {
        polar_Log("ERROR: Cannot update position of source %llu\n", SourceUID);
        return;
    }


    polar_source_CopyToContainer(Source, Sources, i);

    return; 
}


void polar_source_Play(POLAR_MIXER *Mixer, u64 SourceUID, f32 Duration, u32 FX, u32 EnvelopeType, ...)
{
    u32 i = 0;
    POLAR_SOURCE *Sources = polar_source_Retrieval(Mixer, SourceUID, i);
    if(!Sources)
    {
        polar_Log("ERROR: Cannot play source %llu\n", SourceUID);
        return;
    }

    POLAR_SOURCE_SOLO Source = polar_source_CopyFromContainer(Sources, i);

    if(Source.UID)
    {
        if(Source.Type.Flag == SO_FILE)
        {
            if(Duration <= 0)
            {
                Source.SampleCount = Source.Type.File->FrameCount;
            }

            else
            {   
                Source.SampleCount = (Source.SampleRate * Duration);
            }
        }

        if(Source.Type.Flag == SO_OSCILLATOR)
        {
            Source.SampleCount = (Source.SampleRate * Duration);
        }

        switch(EnvelopeType)
        {
            case EN_NONE:
            {
                va_list ArgList;
                va_start(ArgList, EnvelopeType);

                f64 AmpNew = va_arg(ArgList, f64);

                Source.States.Amplitude.Current = AmpNew;
                Source.States.Amplitude.Previous = Source.States.Amplitude.Current;

                va_end(ArgList);
                break;
            }

            case EN_ADSR:
            {
                va_list ArgList;
                va_start(ArgList, EnvelopeType);

                Source.States.Envelope[0].Assignment = EN_AMPLITUDE;
                Source.States.Envelope[0].Points[0].Time = 0.5;
                Source.States.Envelope[0].Points[1].Time = 0.1;
                Source.States.Envelope[0].Points[2].Time = 1.3;
                Source.States.Envelope[0].Points[3].Time = 0.6;
                Source.States.Envelope[0].Points[0].Value = AMP(-1);
                Source.States.Envelope[0].Points[1].Value = AMP(-4);
                Source.States.Envelope[0].Points[2].Value = AMP(-1);
                Source.States.Envelope[0].Points[3].Value = AMP(-8);
                Source.States.Envelope[0].CurrentPoints = 4;
                Source.States.Envelope[0].Index = 0;
                Source.States.CurrentEnvelopes = 1;

                va_end(ArgList);
                break;
            }

            case EN_BREAKPOINT:
            {
                va_list ArgList;
                va_start(ArgList, EnvelopeType);
                char *FileName = va_arg(ArgList, char *);

                FILE *BreakpointFile;

                char FilePath[MAX_STRING_LENGTH];
                polar_StringConcatenate(StringLength(AssetPath), AssetPath, StringLength(FileName), FileName, FilePath);
#if _WIN32    
                fopen_s(&BreakpointFile, FilePath, "r");
#else
                BreakpointFile = fopen(FilePath, "r");
#endif
                polar_envelope_BreakpointsFromFile(BreakpointFile, Source.States.Envelope[0], Source.States.Envelope[1]);
                Source.States.CurrentEnvelopes = 2;

                Source.States.Amplitude.Current = 0;
                Source.States.Amplitude.Previous = Source.States.Amplitude.Current;

                u64 TimeEnd = (Source.States.Envelope[0].CurrentPoints - 1);
                Source.SampleCount *= Source.States.Envelope[0].Points[TimeEnd].Time;
                

                va_end(ArgList);
                break;
            }

            default:
            {
                Source.States.Amplitude.Current = DEFAULT_AMPLITUDE;
                Source.States.Amplitude.Previous = Source.States.Amplitude.Current;

                break;
            }
        }

        Source.FX = FX;
        Source.PlayState = Playing;


        Source.States.DistanceFromListener = 0.0f;
        Source.States.DistanceAttenuation = 0.0f;
        Source.States.MinDistance = 1.0f;
        Source.States.MaxDistance = 100.0f;
        Source.States.Rolloff = 1.0f;
        Source.States.RolloffDirty = true; //!Must be set true if min/max or rolloff are changed!
        Source.States.IsDistanceAttenuated = true;

        Source.States.Pan = 0.0f;

    }

    else
    {
        polar_Log("ERROR: Cannot play source %llu\n", SourceUID);
        return;
    }

    polar_source_CopyToContainer(Source, Sources, i);
    
    return;
}



void polar_container_Play(POLAR_MIXER *Mixer, u64 ContainerUID, f32 Duration, u32 FX, u32 EnvelopeType, ...)
{
    u32 Index = 0;

    POLAR_SOURCE *Sources = polar_source_Retrieval(Mixer, ContainerUID, Index);
    if(!Sources)
    {
        polar_Log("ERROR: Cannot play container %llu\n", ContainerUID);
        return;
    }

    for(u32 i = 0; i < Sources->CurrentSources; i++)
    {
        if(Sources->UID[i])
        {
            switch(EnvelopeType)
            {
                case EN_NONE:
                {
                    va_list ArgList;
                    va_start(ArgList, EnvelopeType);

                    f64 AmpNew = va_arg(ArgList, f64);

                    polar_source_Play(Mixer, Sources->UID[i], Duration, FX, EnvelopeType, AmpNew);

                    va_end(ArgList);
                    break;
                }

                default:
                {
                    break;
                }
            }
        }
    }

    return;
}



void polar_container_Fade(POLAR_MIXER *Mixer, u64 ContainerUID, f64 GlobalTime, f32 NewAmplitude, f32 Duration)
{
    u32 Index = 0;

    POLAR_SOURCE *Sources = polar_source_Retrieval(Mixer, ContainerUID, Index);
    if(!Sources)
    {
        polar_Log("ERROR: Cannot play container %llu\n", ContainerUID);
        return;
    }

    for(u32 i = 0; i < Sources->CurrentSources; i++)
    {
        if(Sources->UID[i])
        {
            polar_source_Fade(Mixer, Sources->UID[i], GlobalTime, NewAmplitude, Duration);
        }
    }

    return;
}


#endif