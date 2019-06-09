#include <vector>
#include "SDLGLGraphicsContext.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"
#include "base/NativeApp.h"
#include "base/display.h"
#include "gfx_es2/gpu_features.h"
#include "thin3d/thin3d_create.h"

#if defined(USING_EGL)
#include "EGL/egl.h"
#endif

class GLRenderManager;

#if defined(USING_EGL)

// TODO: Move these into the class.
static EGLDisplay               g_eglDisplay    = NULL;
static EGLContext               g_eglContext    = NULL;
static EGLSurface               g_eglSurface    = NULL;
#ifdef USING_FBDEV
static EGLNativeDisplayType     g_Display       = NULL;
#else
static Display*                 g_Display       = NULL;
#endif
static NativeWindowType         g_Window        = (NativeWindowType)NULL;

int CheckEGLErrors(const char *file, int line) {
	EGLenum error;
	const char *errortext = "unknown";
	error = eglGetError();
	switch (error)
	{
		case EGL_SUCCESS: case 0:           return 0;
		case EGL_NOT_INITIALIZED:           errortext = "EGL_NOT_INITIALIZED"; break;
		case EGL_BAD_ACCESS:                errortext = "EGL_BAD_ACCESS"; break;
		case EGL_BAD_ALLOC:                 errortext = "EGL_BAD_ALLOC"; break;
		case EGL_BAD_ATTRIBUTE:             errortext = "EGL_BAD_ATTRIBUTE"; break;
		case EGL_BAD_CONTEXT:               errortext = "EGL_BAD_CONTEXT"; break;
		case EGL_BAD_CONFIG:                errortext = "EGL_BAD_CONFIG"; break;
		case EGL_BAD_CURRENT_SURFACE:       errortext = "EGL_BAD_CURRENT_SURFACE"; break;
		case EGL_BAD_DISPLAY:               errortext = "EGL_BAD_DISPLAY"; break;
		case EGL_BAD_SURFACE:               errortext = "EGL_BAD_SURFACE"; break;
		case EGL_BAD_MATCH:                 errortext = "EGL_BAD_MATCH"; break;
		case EGL_BAD_PARAMETER:             errortext = "EGL_BAD_PARAMETER"; break;
		case EGL_BAD_NATIVE_PIXMAP:         errortext = "EGL_BAD_NATIVE_PIXMAP"; break;
		case EGL_BAD_NATIVE_WINDOW:         errortext = "EGL_BAD_NATIVE_WINDOW"; break;
		default:                            errortext = "unknown"; break;
	}
	printf( "ERROR: EGL Error %s detected in file %s at line %d (0x%X)\n", errortext, file, line, error );
	return 1;
}

#define EGL_ERROR(str, check) { \
		if (check) CheckEGLErrors( __FILE__, __LINE__ ); \
		printf("EGL ERROR: " str "\n"); \
		return 1; \
	}

int8_t EGL_Open() {
#ifdef USING_FBDEV
	g_Display = ((EGLNativeDisplayType)0);
#else
	if ((g_Display = XOpenDisplay(NULL)) == NULL)
		EGL_ERROR("Unable to get display!", false);
#endif
	if ((g_eglDisplay = eglGetDisplay((NativeDisplayType)g_Display)) == EGL_NO_DISPLAY)
		EGL_ERROR("Unable to create EGL display.", true);
	if (eglInitialize(g_eglDisplay, NULL, NULL) != EGL_TRUE)
		EGL_ERROR("Unable to initialize EGL display.", true);
	return 0;
}

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR (1 << 6)
#endif

EGLConfig EGL_FindConfig(int *contextVersion) {
	std::vector<EGLConfig> configs;
	EGLint numConfigs = 0;

	EGLBoolean result = eglGetConfigs(g_eglDisplay, nullptr, 0, &numConfigs);
	if (result != EGL_TRUE || numConfigs == 0) {
		return nullptr;
	}

	// prepare for making a table of the EGL heuristics scores
	enum {
		CONFIGNUM,
		COLORSCORE,
		ALPHASCORE,
		DEPTHSCORE,
		STENCILSCORE,
		LEVELSCORE,
		SAMPLESSCORE,
		SAMPLEBUFFERSCORE,
		TRANSPARENTSCORE,
		CAVEATSCORE,
		SURFACESCORE,
		RENDERABLESCOREGLES,
		RENDERABLESCOREGL,
		TOTALSCORE,
		SCORETYPESSIZE
	};
	std::vector<std::vector<int> > scoresTable(numConfigs, std::vector<int>(SCORETYPESSIZE));
	std::string scoresTableHeadings[SCORETYPESSIZE] = {
		"Config:",
		"Colour:",
		"Alpha:",
		"Depth:",
		"Stencl:",
		"Level:",
		"Sample:",
		"Buffer:",
		"Trans:",
		"Caveat:",
		"Surf:",
		"GLES:",
		"GL:",
		"TOTAL:"
	};
	int scoresTableColumn = 0;

	configs.resize(numConfigs);
	result = eglGetConfigs(g_eglDisplay, &configs[0], numConfigs, &numConfigs);
	if (result != EGL_TRUE || numConfigs == 0) {
		return nullptr;
	}

	// Mali (ARM) seems to have compositing issues with alpha backbuffers.
	// EGL_TRANSPARENT_TYPE doesn't help.
	const char *vendorName = eglQueryString(g_eglDisplay, EGL_VENDOR);
	const bool avoidAlphaGLES = vendorName && !strcmp(vendorName, "ARM");

	EGLConfig best = nullptr;
	int bestScore = 0;
	int bestContextVersion = 0;
	int bestConfigNum = 0;
	for (const EGLConfig &config : configs) {
		auto readConfig = [&](EGLint attr) -> EGLint {
			EGLint val = 0;
			eglGetConfigAttrib(g_eglDisplay, config, attr, &val);
			return val;
		};

		// We don't want HDR modes with more than 8 bits per component.
		auto readConfigMax = [&](EGLint attr, EGLint m) -> EGLint {
			EGLint val = readConfig(attr);
			return val > m ? 1 : val;  // why not 0?
		};
		scoresTable[scoresTableColumn][CONFIGNUM] = scoresTableColumn+1;
		int colorScore = readConfigMax(EGL_RED_SIZE, 8) + readConfigMax(EGL_BLUE_SIZE, 8) + readConfigMax(EGL_GREEN_SIZE, 8);
		scoresTable[scoresTableColumn][COLORSCORE] = colorScore;
		int alphaScore = readConfigMax(EGL_ALPHA_SIZE, 8);
		int depthScore = readConfig(EGL_DEPTH_SIZE);
		scoresTable[scoresTableColumn][DEPTHSCORE] = depthScore;
		int levelScore = readConfig(EGL_LEVEL) == 0 ? 100 : 0;
		scoresTable[scoresTableColumn][LEVELSCORE] = levelScore;
		int samplesScore = readConfig(EGL_SAMPLES) == 0 ? 100 : 0;
		scoresTable[scoresTableColumn][SAMPLESSCORE] = samplesScore;
		int sampleBufferScore = readConfig(EGL_SAMPLE_BUFFERS) == 0 ? 100 : 0;
		scoresTable[scoresTableColumn][SAMPLEBUFFERSCORE] = sampleBufferScore;
		int stencilScore = readConfig(EGL_STENCIL_SIZE);
		scoresTable[scoresTableColumn][STENCILSCORE] = stencilScore;
		int transparentScore = readConfig(EGL_TRANSPARENT_TYPE) == EGL_NONE ? 50 : 0;
		scoresTable[scoresTableColumn][TRANSPARENTSCORE] = transparentScore;

		EGLint caveat = readConfig(EGL_CONFIG_CAVEAT);
		int caveatScore = caveat == EGL_NONE ? 100 : (caveat == EGL_NON_CONFORMANT_CONFIG ? 50 : 0);
		//int caveatScore = caveat == EGL_NONE ? 100 : (caveat == EGL_NON_CONFORMANT_CONFIG ? 99 : 0);
		scoresTable[scoresTableColumn][CAVEATSCORE] = caveatScore;

#ifndef USING_FBDEV
		EGLint surfaceType = readConfig(EGL_SURFACE_TYPE);
		int surfaceScore = (surfaceType & EGL_WINDOW_BIT) ? 100 : 0;
		scoresTable[scoresTableColumn][SURFACESCORE] = surfaceScore;
#else
		scoresTable[scoresTableColumn][SURFACESCORE] = 0;
#endif

		EGLint renderable = readConfig(EGL_RENDERABLE_TYPE);
		bool renderableGLES3 = (renderable & EGL_OPENGL_ES3_BIT_KHR) != 0;
		bool renderableGLES2 = (renderable & EGL_OPENGL_ES2_BIT) != 0;
		bool renderableGL = (renderable & EGL_OPENGL_BIT) != 0;
#ifdef USING_GLES2
		int renderableScoreGLES = renderableGLES3 ? 100 : (renderableGLES2 ? 80 : 0);
		int renderableScoreGL = 0;
#else
		int renderableScoreGLES = 0;
		int renderableScoreGL = renderableGL ? 100 : (renderableGLES3 ? 80 : 0);
#endif
		scoresTable[scoresTableColumn][RENDERABLESCOREGLES] = renderableScoreGLES;
		scoresTable[scoresTableColumn][RENDERABLESCOREGL] = renderableScoreGL;

		if (avoidAlphaGLES && renderableScoreGLES > 0) {
			alphaScore = 8 - alphaScore;
		}
		scoresTable[scoresTableColumn][ALPHASCORE] = alphaScore;

		int score = 0;
		// Here's a good place to play with the weights to pick a better config.
		score += colorScore * 10 + alphaScore * 2;
		score += depthScore * 5 + stencilScore;
		score += levelScore + samplesScore + sampleBufferScore + transparentScore;
		score += caveatScore + renderableScoreGLES + renderableScoreGL;

#ifndef USING_FBDEV
		score += surfaceScore;
#endif

		scoresTable[scoresTableColumn][TOTALSCORE] = score;

		if (score > bestScore) {
			bestScore = score;
			best = config;
			bestContextVersion = renderableGLES3 ? 3 : (renderableGLES2 ? 2 : 0);
			bestConfigNum = scoresTableColumn+1;
		}

		scoresTableColumn++;
	}

	printf("\nEGL Config Scores:\n");
	for (int scoresTableRow = 0; scoresTableRow < SCORETYPESSIZE; scoresTableRow++) {
		printf("%s\t", scoresTableHeadings[scoresTableRow].c_str());
		for (int scoresTableColumn = 0; scoresTableColumn < numConfigs; scoresTableColumn++) {
			printf("%d\t", scoresTable[scoresTableColumn][scoresTableRow]);
		}
		printf("\n");
	}
	printf("\nHeuristics chose config #%d\n\n", bestConfigNum);

	*contextVersion = bestContextVersion;
	return best;
}

int8_t EGL_Init(SDL_Window *window) {
	int contextVersion = 0;
	EGLConfig eglConfig = EGL_FindConfig(&contextVersion);
	if (!eglConfig) {
		EGL_ERROR("Unable to find a usable EGL config.", true);
		return 1;
	}

	EGLint contextAttributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, contextVersion,
		EGL_NONE,
	};
	if (contextVersion == 0) {
		contextAttributes[0] = EGL_NONE;
	}

	g_eglContext = eglCreateContext(g_eglDisplay, eglConfig, nullptr, contextAttributes);
	if (g_eglContext == EGL_NO_CONTEXT) {
		EGL_ERROR("Unable to create GLES context!", true);
		return 1;
	}

#if !defined(USING_FBDEV) && !defined(__APPLE__)
	// Get the SDL window handle
	SDL_SysWMinfo sysInfo; //Will hold our Window information
	SDL_VERSION(&sysInfo.version); //Set SDL version
	SDL_GetWindowWMInfo(window, &sysInfo);
	g_Window = (NativeWindowType)sysInfo.info.x11.window;
#else
	g_Window = (NativeWindowType)NULL;
#endif
	g_eglSurface = eglCreateWindowSurface(g_eglDisplay, eglConfig, g_Window, nullptr);
	if (g_eglSurface == EGL_NO_SURFACE) {
		EGL_ERROR("Unable to create EGL surface!", true);
		return 1;
	}

	if (eglMakeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface, g_eglContext) != EGL_TRUE) {
		EGL_ERROR("Unable to make GLES context current.", true);
		return 1;
	}

	return 0;
}

void EGL_Close() {
	if (g_eglDisplay != NULL) {
		eglMakeCurrent(g_eglDisplay, NULL, NULL, EGL_NO_CONTEXT);
		if (g_eglContext != NULL) {
			eglDestroyContext(g_eglDisplay, g_eglContext);
		}
		if (g_eglSurface != NULL) {
			eglDestroySurface(g_eglDisplay, g_eglSurface);
		}
		eglTerminate(g_eglDisplay);
		g_eglDisplay = NULL;
	}
	if (g_Display != NULL) {
#if !defined(USING_FBDEV)
		XCloseDisplay(g_Display);
#endif
		g_Display = NULL;
	}
	g_eglSurface = NULL;
	g_eglContext = NULL;
}

#endif // USING_EGL

// Returns 0 on success.
int SDLGLGraphicsContext::Init(SDL_Window *&window, int x, int y, int mode, std::string *error_message) {
	struct GLVersionPair {
		int major;
		int minor;
	};
	GLVersionPair attemptVersions[] = {
#ifdef USING_GLES2
		{3, 2}, {3, 1}, {3, 0}, {2, 0},
#else
		{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0},
		{3, 3}, {3, 2}, {3, 1}, {3, 0},
#endif
	};

	// We start hidden because we have to try several windows.
	// On Mac, full screen animates so each attempt is slow.
	mode |= SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;

	SDL_GLContext glContext = nullptr;
	for (size_t i = 0; i < ARRAY_SIZE(attemptVersions); ++i) {
		const auto &ver = attemptVersions[i];
		// Make sure to request a somewhat modern GL context at least - the
		// latest supported by MacOS X (really, really sad...)
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, ver.major);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, ver.minor);
#ifdef USING_GLES2
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
		SetGLCoreContext(false);
#else
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SetGLCoreContext(true);
#endif

		window = SDL_CreateWindow("PPSSPP", x,y, pixel_xres, pixel_yres, mode);
		if (!window) {
			// Definitely don't shutdown here: we'll keep trying more GL versions.
			fprintf(stderr, "SDL_CreateWindow failed for GL %d.%d: %s\n", ver.major, ver.minor, SDL_GetError());
			// Skip the DestroyWindow.
			continue;
		}

		glContext = SDL_GL_CreateContext(window);
		if (glContext != nullptr) {
			// Victory, got one.
			break;
		}

		// Let's keep trying.  To be safe, destroy the window - docs say needed to change profile.
		// in practice, it doesn't seem to matter, but maybe it differs by platform.
		SDL_DestroyWindow(window);
	}

	if (glContext == nullptr) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 0);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		SetGLCoreContext(false);

		window = SDL_CreateWindow("PPSSPP", x,y, pixel_xres, pixel_yres, mode);
		if (window == nullptr) {
			NativeShutdown();
			fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
			SDL_Quit();
			return 2;
		}

		glContext = SDL_GL_CreateContext(window);
		if (glContext == nullptr) {
			NativeShutdown();
			fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
			SDL_Quit();
			return 2;
		}
	}

	// At this point, we have a window that we can show finally.
	SDL_ShowWindow(window);

#ifdef USING_EGL
	if (EGL_Init(window) != 0) {
		printf("EGL_Init() failed\n");
		return 1;
	}
#endif

#ifndef USING_GLES2
	// Some core profile drivers elide certain extensions from GL_EXTENSIONS/etc.
	// glewExperimental allows us to force GLEW to search for the pointers anyway.
	if (gl_extensions.IsCoreContext) {
		glewExperimental = true;
	}
	if (GLEW_OK != glewInit()) {
		printf("Failed to initialize glew!\n");
		return 1;
	}
	// Unfortunately, glew will generate an invalid enum error, ignore.
	if (gl_extensions.IsCoreContext)
		glGetError();

	if (GLEW_VERSION_2_0) {
		printf("OpenGL 2.0 or higher.\n");
	} else {
		printf("Sorry, this program requires OpenGL 2.0.\n");
		return 1;
	}
#endif

	// Finally we can do the regular initialization.
	CheckGLExtensions();
	draw_ = Draw::T3DCreateGLContext();
	renderManager_ = (GLRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
	SetGPUBackend(GPUBackend::OPENGL);
	bool success = draw_->CreatePresets();
	assert(success);
	renderManager_->SetSwapFunction([&]() {
#ifdef USING_EGL
		eglSwapBuffers(g_eglDisplay, g_eglSurface);
#else
		SDL_GL_SwapWindow(window_);
#endif
	});
	window_ = window;
	return 0;
}

void SDLGLGraphicsContext::Shutdown() {
}

void SDLGLGraphicsContext::ShutdownFromRenderThread() {
	delete draw_;
	draw_ = nullptr;

#ifdef USING_EGL
	EGL_Close();
#else
	SDL_GL_DeleteContext(glContext);
#endif
}
