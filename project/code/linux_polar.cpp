//Polar
#include "polar.h"
#include "polar_file.cpp"
#include "polar_dsp.cpp"

//Linux
#include "linux_polar.h"

//Linux globals
global bool GlobalRunning;
global bool GlobalPause;
global LINUX_OFFSCREEN_BUFFER GlobalDisplayBuffer;

//!Test variables!
global WAVEFORM Waveform = SINE;

//ALSA setup
ALSA_DATA *linux_ALSA_Create(POLAR_BUFFER &Buffer, u32 UserSampleRate, u16 UserChannels, u32 UserLatency)
{
    //Error handling code passed to snd_strerror()
    i32 ALSAError;

    ALSA_DATA *Result = (ALSA_DATA *) mmap(nullptr, (sizeof (ALSA_DATA)), PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);;
    Result->SampleRate = UserSampleRate;
    Result->ALSAResample = 1;
    Result->Channels = UserChannels;
    Result->LatencyInMS = UserLatency;

    ALSAError = snd_pcm_open(&Result->Device, "default", SND_PCM_STREAM_PLAYBACK, 0);   
    ERR_TO_RETURN(ALSAError, "Failed to open default audio device", nullptr);

    ALSAError = snd_pcm_set_params(Result->Device, SND_PCM_FORMAT_FLOAT, SND_PCM_ACCESS_RW_INTERLEAVED, Result->Channels, Result->SampleRate, Result->ALSAResample, (Result->LatencyInMS * 1000));
    ERR_TO_RETURN(ALSAError, "Failed to set default device parameters", nullptr);

    ALSAError = snd_pcm_get_params(Result->Device, &Result->BufferSize, &Result->PeriodSize);
    ERR_TO_RETURN(ALSAError, "Failed to get default device parameters", nullptr);

    Buffer.SampleBuffer = (f32 *) mmap(nullptr, ((sizeof *Buffer.SampleBuffer) * (Result->SampleRate * Result->Channels)), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Buffer.DeviceBuffer = (f32 *) mmap(nullptr, ((sizeof *Buffer.SampleBuffer) * (Result->SampleRate * Result->Channels)), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Buffer.FramesAvailable = ((Result->BufferSize + Result->PeriodSize) * Result->Channels);

    return Result;
}

//ALSA destroy
void linux_ALSA_Destroy(ALSA_DATA *Result, POLAR_BUFFER &Buffer)
{
    munmap(Buffer.SampleBuffer, (sizeof *Buffer.SampleBuffer) * (Result->SampleRate * Result->Channels));
    munmap(Buffer.DeviceBuffer, (sizeof *Buffer.SampleBuffer) * (Result->SampleRate * Result->Channels));
    snd_pcm_close(Result->Device);
    munmap(Result, (sizeof (Result)));
}


//Linux file handling
//Find file name of current application
internal void linux_EXEFileNameGet(LINUX_STATE *State)
{
    //Read value of a symbolic link and record size
    ssize_t PathSize = readlink("/proc/self/exe", State->EXEPath, ArrayCount(State->EXEPath) - 1);
    if(PathSize > 0)
    {
        State->EXEFileName = State->EXEPath;

        //Scan through the full path and record
        for(char *Scan = State->EXEPath; *Scan; ++Scan)
        {
            if(*Scan == '\\')
            {
                State->EXEFileName = Scan + 1;
            }
        }
    }
}

//Get file path
internal void linux_BuildEXEPathGet(LINUX_STATE *State, const char *FileName, char *Path)
{
    polar_StringConcatenate(State->EXEFileName - State->EXEPath, State->EXEPath, polar_StringLengthGet(FileName), FileName, Path);
}

//Record file attributes using stat ("http://pubs.opengroup.org/onlinepubs/000095399/basedefs/sys/stat.h.html") 
internal ino_t linux_FileIDGet(char *FileName)
{
    struct stat FileAttributes = {};

    if(stat(FileName, &FileAttributes))
    {
        FileAttributes.st_ino = 0;
    }

    return FileAttributes.st_ino;
}

//Wrap dlopen with error handling
internal void *linux_LibraryOpen(const char *Library)
{
    void *Handle = nullptr;

    Handle = dlopen(Library, RTLD_NOW | RTLD_LOCAL);
    
    //Record error using dlerror
    if(!Handle)
    {
        printf("Linux: dlopen failed!\t%s\n", dlerror());
    }

    return Handle;
}

//Wrap dlclose
internal void linux_LibraryClose(void *Handle)
{
    if(Handle != nullptr)
    {
        dlclose(Handle);
        Handle = nullptr;
    }
}

//Wrap dlsym with error handling
internal void *linux_ExternalFunctionLoad(void *Library, const char *Name)
{
    void *FunctionSymbol = dlsym(Library, Name);

    if(!FunctionSymbol)
    {
        printf("Linux: dlsym failed!\t%s\n", dlerror());
    }

    return FunctionSymbol;
}

//Check if file ID's match and load engine code if not
internal bool linux_EngineCodeLoad(LINUX_ENGINE_CODE *EngineCode, char *DLName, ino_t FileID)
{
    if(EngineCode->EngineID != FileID)
    {
        linux_LibraryClose(EngineCode->EngineHandle);
        EngineCode->EngineID = FileID;
        EngineCode->IsDLValid = false;

        //TODO: Can't actually pass DLName here because Linux want's "./" prefixed, create function to prefix strings
        EngineCode->EngineHandle = linux_LibraryOpen("./polar.so");
        if (EngineCode->EngineHandle)
        {
            *(void **)(&EngineCode->UpdateAndRender) = linux_ExternalFunctionLoad(EngineCode->EngineHandle, "RenderUpdate");

            EngineCode->IsDLValid = (EngineCode->UpdateAndRender);
        }
    }

    if(!EngineCode->IsDLValid)
    {
        linux_LibraryClose(EngineCode->EngineHandle);
        EngineCode->EngineID = 0;
        EngineCode->UpdateAndRender = 0;
    }

    return EngineCode->IsDLValid;
}

//Unload engine code
internal void linux_EngineCodeUnload(LINUX_ENGINE_CODE *EngineCode)
{
    linux_LibraryClose(EngineCode->EngineHandle);
    EngineCode->EngineID = 0;
    EngineCode->IsDLValid = false;
    EngineCode->UpdateAndRender = 0;
}

//Process inputs when released
internal void linux_InputMessageProcess(POLAR_INPUT_STATE *NewState, bool IsDown)
{
    if(NewState->EndedDown != IsDown)
    {
        NewState->EndedDown = IsDown;
        ++NewState->HalfTransitionCount;
    }
}

//Process the window message queue
internal void linux_WindowMessageProcess(LINUX_STATE *State, Display *Display, Window Window, Atom WmDeleteWindow, POLAR_INPUT_CONTROLLER *KeyboardController)
{
    while(GlobalRunning && XPending(Display))
    {
        XEvent Event;
        XNextEvent(Display, &Event);

        switch(Event.type)
        {
            case ConfigureNotify:
            case DestroyNotify:
            {
                GlobalRunning = false;
                break;
            }
            case ClientMessage:
            {
                if ((Atom)Event.xclient.data.l[0] == WmDeleteWindow)
                {
                    GlobalRunning = false;
                }
                break;
            }
            case MotionNotify:
            case ButtonRelease:
            case ButtonPress:
            case KeyPress:
            case KeyRelease:
            {
                if(!GlobalPause)
                {
                    if(Event.xkey.keycode == KEYCODE_W)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.MoveUp, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_A)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.MoveLeft, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_S)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.MoveDown, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_D)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.MoveRight, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_Q)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.LeftShoulder, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_E)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.RightShoulder, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_UP)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.ActionUp, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_LEFT)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.ActionLeft, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_DOWN)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.ActionDown, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_RIGHT)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.ActionRight, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_ESCAPE)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.Start, Event.type == KeyRelease);
                    }
                    else if(Event.xkey.keycode == KEYCODE_SPACE)
                    {
                        linux_InputMessageProcess(&KeyboardController->State.Press.Back, Event.type == KeyRelease);
                    }
                }
                
                else if(Event.xkey.keycode == KEYCODE_P)
                {
                    if(Event.type == KeyRelease)
                    {
                        GlobalPause = !GlobalPause;
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

//Create display buffer
internal LINUX_OFFSCREEN_BUFFER linux_WindowDimensionsGet(u32 Width, u32 Height)
{
    LINUX_OFFSCREEN_BUFFER Buffer = {};
    Buffer.Width = Width;
    Buffer.Height = Height;
    Buffer.Pitch = Align16(Buffer.Width * Buffer.BytesPerPixel);
    u32 Size = Buffer.Pitch * Buffer.Height;

    Buffer.Data = (u8 *) mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if(Buffer.Data == MAP_FAILED)
    {
        Buffer.Width = 0;
        Buffer.Height = 0;
        return Buffer;
    }

    return Buffer;
}

int main(int argc, char *argv[])
{
    LINUX_STATE LinuxState = {};
    linux_EXEFileNameGet(&LinuxState);
    linux_BuildEXEPathGet(&LinuxState, "polar.so", LinuxState.EngineSourceCodePath);

    GlobalDisplayBuffer = linux_WindowDimensionsGet(1280, 720);
    Display *X11Display = XOpenDisplay(":0.0");

    if(X11Display)
    {
  	    i32 Screen = DefaultScreen(X11Display);
        Window X11Window = XCreateSimpleWindow(X11Display, DefaultRootWindow(X11Display), 0, 0, 1280, 720, 5, WhitePixel(X11Display, Screen), BlackPixel(X11Display, Screen));
        
        //!Inputs need this to process but are stuck in infinte loop (and Pause key doesn't work), debug this!
        // XSelectInput(X11Display, X11Window, ExposureMask|KeyReleaseMask);

        if(X11Window)
        {
            GC GraphicsContext = XCreateGC(X11Display, X11Window, 0,0); 

            XSetBackground(X11Display, GraphicsContext, WhitePixel(X11Display, Screen));
	        XSetForeground(X11Display, GraphicsContext, BlackPixel(X11Display, Screen));
            
            XSizeHints SizeHints = {};
            SizeHints.x = 0;
            SizeHints.y = 0;
            SizeHints.width  = GlobalDisplayBuffer.Width;
            SizeHints.height = GlobalDisplayBuffer.Height;
            SizeHints.flags = USSize | USPosition;

            XSetNormalHints(X11Display, X11Window, &SizeHints);
            XSetStandardProperties(X11Display, X11Window, "Polar", "glsync text", None, nullptr, 0, &SizeHints);

            Atom WmDeleteWindow = XInternAtom(X11Display, "WM_DELETE_WINDOW", False);
            XSetWMProtocols(X11Display, X11Window, &WmDeleteWindow, 1);

            POLAR_DATA PolarEngine = {};
            ALSA_DATA *ALSA =           linux_ALSA_Create(PolarEngine.Buffer, 48000, 2, 32);
            PolarEngine.BufferFrames =  PolarEngine.Buffer.FramesAvailable;
            PolarEngine.Channels =      ALSA->Channels;
            PolarEngine.SampleRate =    ALSA->SampleRate;
            //TODO: Convert flags like SND_PCM_FORMAT_FLOAT to numbers
            PolarEngine.BitRate =       32;

            //Start infinite loop
            GlobalRunning = true;

            POLAR_WAV *OutputRenderFile = polar_render_WAVWriteCreate("Polar_Output.wav", &PolarEngine);
            
            //Create objects
            //!Move all of this into a memory arena
            POLAR_PLAYING_SOUND *TestSine = (POLAR_PLAYING_SOUND *) mmap(nullptr, (sizeof (POLAR_PLAYING_SOUND)), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            TestSine->UID = 1;
            TestSine->Oscillator = polar_wave_OscillatorCreate(PolarEngine.SampleRate, Waveform, 440);
            TestSine->State = (POLAR_OBJECT_STATE *) mmap(nullptr, (sizeof (POLAR_OBJECT_STATE)), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            TestSine->State->Frequency = 207.65;
            TestSine->State->Amplitude = 0.8;
            TestSine->State->Pan = 0.2;
    
            POLAR_PLAYING_SOUND *TestTriangle = (POLAR_PLAYING_SOUND *) mmap(nullptr, (sizeof (POLAR_PLAYING_SOUND)), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            TestTriangle->UID = 2;
            TestTriangle->Oscillator = polar_wave_OscillatorCreate(PolarEngine.SampleRate, Waveform, 440);
            TestTriangle->State = (POLAR_OBJECT_STATE *) mmap(nullptr, (sizeof (POLAR_OBJECT_STATE)), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            TestTriangle->State->Frequency = 164.81;
            TestTriangle->State->Amplitude = 0.5;
            TestTriangle->State->Pan = 0.2;

            POLAR_PLAYING_SOUND *TestSquare = (POLAR_PLAYING_SOUND *) mmap(nullptr, (sizeof (POLAR_PLAYING_SOUND)), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            TestSquare->UID = 3;
            TestSquare->Oscillator = polar_wave_OscillatorCreate(PolarEngine.SampleRate, Waveform, 440);
            TestSquare->State = (POLAR_OBJECT_STATE *) mmap(nullptr, (sizeof (POLAR_OBJECT_STATE)), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            TestSquare->State->Frequency = 233.08;
            TestSquare->State->Amplitude = 0.6;
            TestSquare->State->Pan = -0.2;

            POLAR_PLAYING_SOUND *Test03 = (POLAR_PLAYING_SOUND *) mmap(nullptr, (sizeof (POLAR_PLAYING_SOUND)), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            Test03->UID = 4;
            Test03->Oscillator = polar_wave_OscillatorCreate(PolarEngine.SampleRate, Waveform, 440);
            Test03->State = (POLAR_OBJECT_STATE *) mmap(nullptr, (sizeof (POLAR_OBJECT_STATE)), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            Test03->State->Frequency = 293.66;
            Test03->State->Amplitude = 0.7;
            Test03->State->Pan = -0.2;

            //Allocate engine memory block
            POLAR_MEMORY EngineMemory = {};
            EngineMemory.PermanentDataSize = Megabytes(32);
            EngineMemory.TemporaryDataSize = Megabytes(8);

            LinuxState.TotalSize = EngineMemory.PermanentDataSize + EngineMemory.TemporaryDataSize;
            LinuxState.EngineMemoryBlock = mmap(nullptr, ((size_t) LinuxState.TotalSize), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

            EngineMemory.PermanentData = LinuxState.EngineMemoryBlock;
            EngineMemory.TemporaryData = ((uint8 *) EngineMemory.PermanentData + EngineMemory.PermanentDataSize);

            if(EngineMemory.PermanentData && EngineMemory.TemporaryData)
            {   
                LINUX_ENGINE_CODE PolarState = {};
                linux_EngineCodeLoad(&PolarState, LinuxState.EngineSourceCodePath, linux_FileIDGet(LinuxState.EngineSourceCodePath));

                POLAR_INPUT Input[2] = {};
                POLAR_INPUT *NewInput = &Input[0];
                POLAR_INPUT *OldInput = &Input[1];

	            XMapRaised(X11Display, X11Window);

                while(GlobalRunning)
                {
                    POLAR_INPUT_CONTROLLER *OldKeyboardController = ControllerGet(OldInput, 0);
                    POLAR_INPUT_CONTROLLER *NewKeyboardController = ControllerGet(NewInput, 0);
                    *NewKeyboardController = {};
                    NewKeyboardController->IsConnected = true;
                    
                    for(u32 ButtonIndex = 0; ButtonIndex < ArrayCount(NewKeyboardController->State.Buttons); ++ButtonIndex)
                    {
                        NewKeyboardController->State.Buttons[ButtonIndex].EndedDown = OldKeyboardController->State.Buttons[ButtonIndex].EndedDown;
                    }

                    for(u32 ButtonIndex = 0; ButtonIndex < 5; ++ButtonIndex)
                    {
                        NewInput->MouseButtons[ButtonIndex] = OldInput->MouseButtons[ButtonIndex];
                        NewInput->MouseButtons[ButtonIndex].HalfTransitionCount = 0;
                    }

                    linux_WindowMessageProcess(&LinuxState, X11Display, X11Window, WmDeleteWindow, NewKeyboardController);

                    if(!GlobalPause)
                    {
                        //Extern rendering function
                        if(PolarState.UpdateAndRender)
                        {
                            //Update objects and fill the buffer
                            if(OutputRenderFile != nullptr)
                            {
                                PolarState.UpdateAndRender(PolarEngine, OutputRenderFile, &EngineMemory, NewInput, TestSine, TestTriangle, TestSquare, Test03);
                                OutputRenderFile->TotalSampleCount += polar_render_WAVWriteFloat(OutputRenderFile, (PolarEngine.Buffer.FramesAvailable * PolarEngine.Channels), OutputRenderFile->Data);
                            }

                            else
                            {
                                PolarState.UpdateAndRender(PolarEngine, nullptr, &EngineMemory, NewInput, TestSine, TestTriangle, TestSquare, Test03);
                            }

                            ALSA->FramesWritten = snd_pcm_writei(ALSA->Device, PolarEngine.Buffer.SampleBuffer, (PolarEngine.BufferFrames));

                            //If no frames are written then try to recover the output stream
                            if(ALSA->FramesWritten < 0)
                            {
                                ALSA->FramesWritten = snd_pcm_recover(ALSA->Device, ALSA->FramesWritten, 0);
                            }

                            //If recovery fails then quit
                            if(ALSA->FramesWritten < 0) 
                            {
                                ERR_TO_RETURN(ALSA->FramesWritten, "ALSA: Failed to write any output frames! snd_pcm_writei()", -1);
                            }

                            //Wrote less frames than the total buffer length
                            if(ALSA->FramesWritten > 0 && ALSA->FramesWritten < (PolarEngine.BufferFrames))
                            {
                                printf("ALSA: Short write!\tExpected %i, wrote %li\n", (PolarEngine.BufferFrames), ALSA->FramesWritten);
                            }

                            printf("ALSA: Frames written:\t%ld\n", ALSA->FramesWritten);
                        }

                        //Reset input for next loop
                        POLAR_INPUT *Temp = NewInput;
                        NewInput = OldInput;
                        OldInput = Temp;
                    }      
                }
            }

            printf("Polar: %lu frames written to %s\n", OutputRenderFile->TotalSampleCount, OutputRenderFile->Path);
    
            polar_render_WAVWriteDestroy(OutputRenderFile);
    

            munmap(LinuxState.EngineMemoryBlock, ((size_t) LinuxState.TotalSize));
            linux_ALSA_Destroy(ALSA, PolarEngine.Buffer);
        }
    }
    
    return 0;
}
