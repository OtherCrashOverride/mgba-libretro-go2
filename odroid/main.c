/*
mgba - GBA port for the ODROID-GO Advance
Copyright (C) 2020  OtherCrashOverride

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <go2/display.h>
#include <go2/audio.h>
#include <go2/input.h>
#include <drm/drm_fourcc.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <stdbool.h>

// mGBA
#define M_CORE_GBA 1
#define M_CORE_GB 1
#include <mgba/core/core.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/gb/interface.h>
#include <mgba/internal/gb/gb.h>
#include <mgba-util/vfs.h>
#include <mgba/core/thread.h>

#include <EGL/egl.h>
#include <GLES/gl.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/glext.h>
#include <pwd.h>
#include <pthread.h>


#define SOUND_FREQUENCY (44100)
#define SOUND_CHANNEL_COUNT (2)
#define SAMPLES (1024)


#define REGION_CART_SRAM (0xE)
#define GB_REGION_EXTERNAL_RAM (0xA)


static struct mLogger logger;
static struct mCore* core;

static go2_display_t* display;
static go2_presenter_t* presenter;
static go2_audio_t* audio;
static go2_input_t* input;
static go2_gamepad_state_t gamepadState;


typedef enum 
{
    GO2_DEVICE_UNKNOWN = 0,
    GO2_DEVICE_GB,
    GO2_DEVICE_SGB,
    GO2_DEVICE_GBC,
    GO2_DEVICE_GBA
} emu_device_t;


static void InitSound()
{
    printf("Sound: SOUND_FREQUENCY=%d\n", SOUND_FREQUENCY);

    audio = go2_audio_create(SOUND_FREQUENCY);
}

static void ProcessAudio(uint8_t* samples, int frameCount)
{
    go2_audio_submit(audio, (const short*)samples, frameCount);
}

static void null_log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args)
{
    char buffer[4096] = {
		0
	};
	
    /*
    	mLOG_FATAL = 0x01,
	mLOG_ERROR = 0x02,
	mLOG_WARN = 0x04,
	mLOG_INFO = 0x08,
	mLOG_DEBUG = 0x10,
	mLOG_STUB = 0x20,
	mLOG_GAME_ERROR = 0x40,
    */
	
	//va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	//va_end(args);

	printf("[LOG LVL %0x] %s\n", level, buffer);
	//fflush(stdout);
}

static void GL_CheckError()
{
#ifdef DEBUG
	int error = glGetError();
	if (error != GL_NO_ERROR)
	{
		printf("GL error: error=0x%x\n", error);
        exit(1);
	}
#endif
}

static const char* FileNameFromPath(const char* fullpath)
{
    // Find last slash
    const char* ptr = strrchr(fullpath,'/');
    if (!ptr)
    {
        ptr = fullpath;
    }
    else
    {
        ++ptr;
    } 

    return ptr;   
}

static char* PathCombine(const char* path, const char* filename)
{
    int len = strlen(path);
    int total_len = len + strlen(filename);

    char* result = NULL;

    if (path[len-1] != '/')
    {
        ++total_len;
        result = (char*)calloc(total_len + 1, 1);
        strcpy(result, path);
        strcat(result, "/");
        strcat(result, filename);
    }
    else
    {
        result = (char*)calloc(total_len + 1, 1);
        strcpy(result, path);
        strcat(result, filename);
    }
    
    return result;
}

static int LoadState(const char* saveName)
{
    FILE* file = fopen(saveName, "rb");
	if (!file)
		return -1;

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);

    if (size < 1) return -1;

    void* ptr = malloc(size);
    if (!ptr) abort();

    size_t count = fread(ptr, 1, size, file);
    if ((size_t)size != count)
    {
        free(ptr);
        abort();
    }

    fclose(file);

    struct VFile* vfm = VFileFromConstMemory(ptr, size);
	bool success = mCoreLoadStateNamed(core, vfm, SAVESTATE_RTC);
	vfm->close(vfm);

    free(ptr);

    return 0;
}

static int LoadSram(const char* sramName)
{
    FILE* file = fopen(sramName, "rb");
	if (!file)
		return -1;

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);

    void* sram = malloc(size);
    if (!sram) abort();

    size_t count = fread(sram, 1, size, file);
    if (count != size)
    {
        abort();
    }

    bool writeback = false;
    if (core->platform(core) == PLATFORM_GB)
    {
        writeback = true;
    }

    core->savedataRestore(core, sram, size, writeback); 

    free(sram);
    fclose(file);

    return 0;
}

static void SaveState(const char* saveName)
{
    struct VFile* vfm = VFileMemChunk(NULL, 0);
	mCoreSaveStateNamed(core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
    size_t size = vfm->size(vfm);

    char* buffer = malloc(size);

	vfm->seek(vfm, 0, SEEK_SET);
	vfm->read(vfm, buffer, size);
	vfm->close(vfm);
    

    FILE* file = fopen(saveName, "wb");
	if (!file)
    {
		abort();
    }

    size_t count = fwrite(buffer, 1, size, file);
    if (count != size)
    {
        abort();
    }

    fclose(file);

    free(buffer);
}

static void SaveSram(const char* sramName, void* sram, size_t sramSize)
{
    FILE* file = fopen(sramName, "wb");
	if (!file)
    {
		abort();
    }

    size_t count = fwrite(sram, 1, sramSize, file);
    if (count != sramSize)
    {
        abort();
    }

    fclose(file);
}

volatile bool isRunning = true;

static void* audio_task(void* arg)
{
    int16_t samples[SAMPLES * 1024];

    struct mCoreThread* thread = (struct mCoreThread*)arg;

    // while(!mCoreThreadHasStarted(&thread))
    // {
    //     usleep(1);
    // }

    while(isRunning)
    {
        mCoreSyncLockAudio(&thread->impl->sync);

        blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), SOUND_FREQUENCY);
        blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), SOUND_FREQUENCY);


        //memset(samples, 0, SAMPLES * 1024 * sizeof(int16_t));
#if 1
        size_t available = blip_samples_avail(core->getAudioChannel(core, 0));

        if (available >= SOUND_FREQUENCY / 60)
        {
            blip_read_samples(core->getAudioChannel(core, 0), samples, available, true);
            blip_read_samples(core->getAudioChannel(core, 1), samples + 1, available, true);

            ProcessAudio(samples, available);
            mCoreSyncConsumeAudio(&thread->impl->sync);
        }
        else
        {
            mCoreSyncUnlockAudio(&thread->impl->sync);
            usleep(1);
        }
#else
        mCoreSyncProduceAudio(&thread->impl->sync, core->getAudioChannel(core, 0), SOUND_FREQUENCY / 60);

        size_t available = blip_samples_avail(core->getAudioChannel(core, 0));
        blip_read_samples(core->getAudioChannel(core, 0), samples, available, true);
        blip_read_samples(core->getAudioChannel(core, 1), samples + 1, available, true);

        ProcessAudio(samples, available);
        //mCoreSyncConsumeAudio(&thread->impl->sync);
#endif
    }

    printf("audio_task exit.\n");
}

int main(int argc, char** argv)
{
    int ret;

    if (argc < 2)
    {
        printf("missing filename.\n");
        return false;
    }

    display = go2_display_create();
    presenter = go2_presenter_create(display, /*DRM_FORMAT_RGB565*/ DRM_FORMAT_RGB565, 0xff080808);

    go2_context_attributes_t attr;
    attr.major = 1;
    attr.minor = 0;
    attr.red_bits = 8;
    attr.green_bits = 8;
    attr.blue_bits = 8;
    attr.alpha_bits = 8;
    attr.depth_bits = 0;
    attr.stencil_bits = 0;

    go2_context_t* context = go2_context_create(display, 480, 320, &attr);
    go2_context_make_current(context);


    const char* filename = argv[1];

	// First, we need to find the mCore that's appropriate for this type of file.
	// If one doesn't exist, it returns NULL and we can't continue.
	core = mCoreFind(filename);
	if (!core) 
    {
		return false;
	}


	// Initialize the received core.
    mCoreInitConfig(core, NULL);
	core->init(core);

    logger.log = null_log;
	mLogSetDefaultLogger(&logger);


	// Get the dimensions required for this core and send them to the client.
	unsigned width, height;
	core->desiredVideoDimensions(core, &width, &height);	//ssize_t bufferSize = width * height * BYTES_PER_PIXEL;
	
    printf("RENDER: width=%d, height=%d\n", width, height);

    
	// Create a video buffer and tell the core to use it.
	// If a core isn't told to use a video buffer, it won't render any graphics.
	// This may be useful in situations where everything except for displayed
	// output is desired.
    void* videoOutputBuffer = malloc(width * height * BYTES_PER_PIXEL);
    if (!videoOutputBuffer)
    {
        printf("go2_surface_map failed.\n");
        abort();
    }

	core->setVideoBuffer(core, videoOutputBuffer, width);
    
    core->setAudioBufferSize(core, SAMPLES);
    blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), SOUND_FREQUENCY);
    blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), SOUND_FREQUENCY);


 	// Tell the core to actually load the file.
    //core->loadROM(core, rom);
	mCoreLoadFile(core, filename);


	// Initialize the configuration system and load any saved settings for
	// this frontend. The second argument to mCoreConfigInit should either be
	// the name of the frontend, or NULL if you're not loading any saved
	// settings from disk.
	//mCoreConfigInit(&core->config, NULL);
    
    struct mCoreOptions opts = {
		.useBios = true,
		.rewindEnable = false,
		.rewindBufferCapacity = 600,
		.audioBuffers = 1024,
		.videoSync = true,
		.audioSync = true,
		.volume = 0x100,
	};

    mCoreConfigSetDefaultIntValue(&core->config, "sgb.borders", true);
    mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "detect");

	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadConfig(core);


    emu_device_t emuDevice;

    if (core->platform(core) == PLATFORM_GB)
    {
        struct GB* gb = core->board;
        GBDetectModel(gb);

        switch (gb->model) 
        {
		case GB_MODEL_AGB:
		case GB_MODEL_CGB:
            emuDevice = GO2_DEVICE_GBC;
			printf("DETECTED: AGB/CGB\n");
			break;

		case GB_MODEL_SGB:
            emuDevice = GO2_DEVICE_SGB;
			printf("DETECTED: SGB\n");
			break;

		case GB_MODEL_DMG:
		default:
            emuDevice = GO2_DEVICE_GB;
			printf("DETECTED: DMG\n");
			break;
		}
    }
    else
    {
        emuDevice = GO2_DEVICE_GBA;
    }
    
	// Take any settings overrides from the command line and make sure they get
	// loaded into the config system, as well as manually overriding the
	// "idleOptimization" setting to ensure cores that can detect idle loops
	// will attempt the detection.
	//applyArguments(args, NULL, &core->config);
	//mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "detect");


	// Tell the core to apply the configuration in the associated config object.
    //mCoreConfigSetIntValue(&core->config, "logLevel", /*mLOG_FATAL*/ 1);
	//mCoreLoadConfig(core);


	// Set our logging level to be the logLevel in the configuration object.
    //int _logLevel = 2;
	//mCoreConfigSetIntValue(&core->config, "logLevel", 5);
 
	// Reset the core. This is needed before it can run.
	core->reset(core);
 

    InitSound();
    input = go2_input_create();


    // Texture
    glEnable(GL_TEXTURE_2D);

    GLuint textureid;
	glGenTextures(1, &textureid);
	GL_CheckError();

	glActiveTexture(GL_TEXTURE0);
	GL_CheckError();

	glBindTexture(GL_TEXTURE_2D, textureid);
	GL_CheckError();

	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	GL_CheckError();

	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	GL_CheckError();

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    GL_CheckError();

    glClearColor(0.03125, 0.03125, 0.03125, 1);
    GL_CheckError();

    glTexImage2D(/*GLenum target*/ GL_TEXTURE_2D,
        /*GLint level*/ 0,
        /*GLint internalformat*/ GL_RGBA,
        /*GLsizei width*/ width,
        /*GLsizei height*/ height,
        /*GLint border*/ 0,
        /*GLenum format*/ GL_RGBA,
        /*GLenum type*/ GL_UNSIGNED_BYTE,
        /*const GLvoid * data*/ videoOutputBuffer);
    GL_CheckError();



    PFNGLDRAWTEXIOESPROC _glDrawTexiOES = NULL;
    _glDrawTexiOES = (PFNGLDRAWTEXIOESPROC) eglGetProcAddress("glDrawTexiOES");
    if(_glDrawTexiOES == NULL)
    {
        printf("eglGetProcAddress failed.\n");
        abort();
    }


    struct mCoreThread thread = {
		.core = core
	};

    mCoreThreadStart(&thread);

    while(!mCoreThreadHasStarted(&thread))
    {
        usleep(1);
    }

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, audio_task, (void*)&thread);


    // Restore
    const char* fileName = FileNameFromPath(filename);
    
    char* saveName = (char*)malloc(strlen(fileName) + 4 + 1);
    strcpy(saveName, fileName);
    strcat(saveName, ".sav");


    struct passwd *pw = getpwuid(getuid());
    const char *homedir = pw->pw_dir;

    char* savePath = PathCombine(homedir, saveName);
    printf("savePath='%s'\n", savePath);
     

    // SRAM
    char* sramName = (char*)malloc(strlen(fileName) + 4 + 1);
    strcpy(sramName, fileName);
    strcat(sramName, ".srm");
    
    char* sramPath = PathCombine(homedir, sramName);
    printf("sramPath='%s'\n", sramPath);
    

    mCoreThreadInterrupt(&thread);
    LoadState(savePath);
    LoadSram(sramPath);    
    mCoreThreadContinue(&thread);


    // Main loop
	while (1) 
    {
		// After receiving the keys from the client, tell the core that these are
		// the keys for the current input.
        go2_gamepad_state_t gamepad;
        go2_input_gamepad_read(input, &gamepad);

        if (gamepad.buttons.f1)
        {
            isRunning = false;
            mCoreThreadEnd(&thread);
            break;
        }
            
        uint32_t keys = 0;
        keys |= (gamepad.buttons.a) << 0;
        keys |= (gamepad.buttons.b) << 1;
        keys |= (gamepad.buttons.f3) << 2;  // SELECT
        keys |= (gamepad.buttons.f4) << 3;  // START
        keys |= (gamepad.dpad.right) << 4;
        keys |= (gamepad.dpad.left) << 5;
        keys |= (gamepad.dpad.up) << 6;
        keys |= (gamepad.dpad.down) << 7;
        keys |= (gamepad.buttons.top_right) << 8;
        keys |= (gamepad.buttons.top_left) << 9;

        const float TRIM = 0.35f;
        keys |= (gamepad.thumb.x < -TRIM) ? (1 << 5) : 0;   // LEFT
        keys |= (gamepad.thumb.x > TRIM) ? (1 << 4) : 0;    // RIGHT
        
        keys |= (gamepad.thumb.y < -TRIM) ? (1 << 6) : 0;   // UP
        keys |= (gamepad.thumb.y > TRIM) ? (1 << 7) : 0;    // DOWN

        mCoreThreadInterrupt(&thread);
        core->setKeys(core, keys);
        mCoreThreadContinue(&thread);


        if (mCoreSyncWaitFrameStart(&thread.impl->sync))
        {
            glClear(GL_COLOR_BUFFER_BIT);
            GL_CheckError();

            glTexSubImage2D(/*GLenum target*/ GL_TEXTURE_2D,
                /*GLint level*/ 0,
                /*xoffset*/ 0,
                /*yoffset*/ 0,
                /*GLsizei width*/ width,
                /*GLsizei height*/ height,
                /*GLenum format*/ GL_RGBA,
                /*GLenum type*/ GL_UNSIGNED_BYTE,
                /*const GLvoid * data*/ videoOutputBuffer);
            GL_CheckError();

            mCoreSyncWaitFrameEnd(&thread.impl->sync);

            GLint params[4];

            switch (emuDevice)
            {
            case GO2_DEVICE_GB:
            case GO2_DEVICE_GBC:
                // render = width=256, height=224
                // LCD 160 × 144 pixels
                // scale = 320 x 288
                params[0] = 0;
                params[1] = 144;
                params[2] = 160; //160;
                params[3] = -144; //-144; // * 2;
                glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, params);
                GL_CheckError();

                _glDrawTexiOES((480 - 320) / 2, (320 - 288) / 2, 0, 320, 288);
                GL_CheckError();
                break;

            case GO2_DEVICE_SGB:
                // render = width=256, height=224
                // 480 x 320 -> 240 x 160
                params[0] = (256 - 240) / 2;
                params[1] = 160 + ((224 - 160) / 2);
                params[2] = 240; 
                params[3] = -160;
                glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, params);
                GL_CheckError();

                _glDrawTexiOES(0, 0, 0, 480, 320);
                GL_CheckError();
                break;

            case GO2_DEVICE_GBA:            
                params[0] = 0;
                params[1] = 0;
                params[2] = width; 
                params[3] = -height;
                glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, params);
                GL_CheckError();

                _glDrawTexiOES(0, 0, 0, 480, 320);
                GL_CheckError();
                break;

            default:
                break;
            }


            go2_context_swap_buffers(context);

            go2_surface_t* surface = go2_context_surface_lock(context);
            go2_presenter_post(presenter,
                        surface,
                        0, 0, 480, 320,
                        0, 0, 320, 480,
                        GO2_ROTATION_DEGREES_270);
            go2_context_surface_unlock(context, surface);           
        }
        else 
        {
            mCoreSyncWaitFrameEnd(&thread.impl->sync);
            usleep(1);
        }
	}

    pthread_join(thread_id, NULL);
    mCoreThreadJoin(&thread);


    // Save
    SaveState(savePath);
    free(savePath);


    void* sram;
    size_t size;
    if (core->platform(core) == PLATFORM_GB)
    {
        sram = core->getMemoryBlock(core, GB_REGION_EXTERNAL_RAM, &size);
    }
    else
    {        
        sram = core->getMemoryBlock(core, REGION_CART_SRAM, &size);
    }

    printf("SAVE: SRAM size=%ld\n", size);

    SaveSram(sramPath, sram, size);
    free(sramPath);


	// Deinitialization associated with the core.
	mCoreConfigDeinit(&core->config);
	core->deinit(core);


    return 0;
}