/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "common/scummsys.h"

#if defined(USE_OPENGL)

#include "backends/graphics/opengl/opengl-graphics.h"
#include "backends/graphics/opengl/glerrorcheck.h"
#include "common/config-manager.h"
#include "common/file.h"
#include "common/mutex.h"
#include "common/textconsole.h"
#include "common/translation.h"
#include "common/advxmlparser.h"
#ifdef USE_OSD
#include "common/tokenizer.h"
#endif
#include "graphics/font.h"
#include "graphics/fontman.h"

OpenGLGraphicsManager::OpenGLGraphicsManager()
	:
#ifdef USE_OSD
	_osdTexture(0), _osdAlpha(0), _osdFadeStartTime(0), _requireOSDUpdate(false),
#endif
	_gameTexture(0), _overlayTexture(0), _cursorTexture(0),
	_screenChangeCount(1 << (sizeof(int) * 8 - 2)), _screenNeedsRedraw(false),
	_shakePos(0),
	_overlayVisible(false), _overlayNeedsRedraw(false),
	_transactionMode(kTransactionNone),
	_cursorNeedsRedraw(false), _cursorPaletteDisabled(true),
	_cursorVisible(false), _cursorKeyColor(0),
	_cursorDontScale(false),
	_formatBGR(false),
	_enableShaders(false), _shadersInited(false), _frameCount(0),
	_displayX(0), _displayY(0), _displayWidth(0), _displayHeight(0) {

	memset(&_oldVideoMode, 0, sizeof(_oldVideoMode));
	memset(&_videoMode, 0, sizeof(_videoMode));
	memset(&_transactionDetails, 0, sizeof(_transactionDetails));

	_scaleMode = OpenGL::GFX_NORMAL;
	_videoMode.mode = 0;
	_videoMode.scaleFactor = 2;
	_videoMode.fullscreen = ConfMan.getBool("fullscreen");
	_videoMode.antialiasing = false;

	_gamePalette = (byte *)calloc(sizeof(byte) * 3, 256);
	_cursorPalette = (byte *)calloc(sizeof(byte) * 3, 256);
}

OpenGLGraphicsManager::~OpenGLGraphicsManager() {
	free(_gamePalette);
	free(_cursorPalette);

	_screenData.free();
	_overlayData.free();
	_cursorData.free();
	_osdSurface.free();

	delete _gameTexture;
	delete _overlayTexture;
	delete _cursorTexture;
}

//
// Feature
//

bool OpenGLGraphicsManager::hasFeature(OSystem::Feature f) {
	return
	    (f == OSystem::kFeatureAspectRatioCorrection) ||
	    (f == OSystem::kFeatureCursorPalette);
}

void OpenGLGraphicsManager::setFeatureState(OSystem::Feature f, bool enable) {
	switch (f) {
	case OSystem::kFeatureFullscreenMode:
		setFullscreenMode(enable);
		break;

	case OSystem::kFeatureAspectRatioCorrection:
		_videoMode.aspectRatioCorrection = enable;
		_transactionDetails.needRefresh = true;
		break;

	case OSystem::kFeatureCursorPalette:
		_cursorPaletteDisabled = !enable;
		_cursorNeedsRedraw = true;
		break;

	default:
		break;
	}
}

bool OpenGLGraphicsManager::getFeatureState(OSystem::Feature f) {
	switch (f) {
	case OSystem::kFeatureFullscreenMode:
		return _videoMode.fullscreen;

	case OSystem::kFeatureAspectRatioCorrection:
		return _videoMode.aspectRatioCorrection;

	case OSystem::kFeatureCursorPalette:
		return !_cursorPaletteDisabled;

	default:
		return false;
	}
}

//
// Screen format and modes
//

#ifdef USE_OSD
static const char *s_scaleModeNames[] = {
	_s("OpenGL Normal"),
	_s("OpenGL Conserve"),
	_s("OpenGL Original")
};
#endif

static Common::Array<OSystem::GraphicsMode> *s_supportedGraphicsModes;

static void initGraphicsModes () {
	s_supportedGraphicsModes = new Common::Array<OSystem::GraphicsMode>;
	OSystem::GraphicsMode gm;
	Common::ArchiveMemberList files;
	SearchMan.listMatchingMembers(files, "*.shader");
	int index = 1;

	// No gl calls can be made since the OpenGL context has not been created yet.
	gm.name = "default";
	gm.id = 0;
	gm.description = "Default";
	s_supportedGraphicsModes->push_back(gm);
	for (Common::ArchiveMemberList::iterator i = files.begin(); i != files.end(); ++i) {
		gm.description = gm.name = strdup((*i)->getName().c_str());
		gm.id = index;
		s_supportedGraphicsModes->push_back(gm);
		index++;
	}
	gm.name = 0;
	gm.description = 0;
	gm.id = 0;
	s_supportedGraphicsModes->push_back(gm);
}

const OSystem::GraphicsMode *OpenGLGraphicsManager::supportedGraphicsModes() {
	if (!s_supportedGraphicsModes)
		initGraphicsModes();
	return &s_supportedGraphicsModes->front();
}

const OSystem::GraphicsMode *OpenGLGraphicsManager::getSupportedGraphicsModes() const {
	if (!s_supportedGraphicsModes)
		initGraphicsModes();
	return &s_supportedGraphicsModes->front();
}

int OpenGLGraphicsManager::getDefaultGraphicsMode() const {
	// First one is default
	return 0;
}

bool OpenGLGraphicsManager::setGraphicsMode(int mode) {
	assert(_transactionMode == kTransactionActive);

	setScale(2);

	if (_oldVideoMode.setup && _oldVideoMode.mode == mode)
		return true;

	if ((uint)mode >= s_supportedGraphicsModes->size() - 1) {
		warning("Unknown gfx mode %d", mode);
		return false;
	}

	_videoMode.mode = mode;
	_transactionDetails.needRefresh = true;

	return true;
}

int OpenGLGraphicsManager::getGraphicsMode() const {
	assert(_transactionMode == kTransactionNone);
	return _videoMode.mode;
}

int OpenGLGraphicsManager::getMaxGraphicsMode() const {
	assert(_transactionMode == kTransactionNone);
	return _shaders.size() - 1;
}

bool OpenGLGraphicsManager::setScaleMode(int scaleMode) {
	assert(_transactionMode == kTransactionActive);

	if (scaleMode > 2 || scaleMode < 0) {
		warning("Unknown scale mode %d", scaleMode);
		return false;
	}

	_scaleMode = scaleMode;
	_transactionDetails.needRefresh = true;

	return true;
}

int OpenGLGraphicsManager::getScaleMode() const {
	return _scaleMode;
}

void OpenGLGraphicsManager::resetGraphicsScale() {
	setScale(1);
}

#ifdef USE_RGB_COLOR
Graphics::PixelFormat OpenGLGraphicsManager::getScreenFormat() const {
	return _screenFormat;
}
#endif

void OpenGLGraphicsManager::initSize(uint width, uint height, const Graphics::PixelFormat *format) {
	assert(_transactionMode == kTransactionActive);

#ifdef USE_RGB_COLOR
	Graphics::PixelFormat newFormat;
	if (!format)
		newFormat = Graphics::PixelFormat::createFormatCLUT8();
	else
		newFormat = *format;

	assert(newFormat.bytesPerPixel > 0);

	// Avoid redundant format changes
	if (newFormat != _videoMode.format) {
		_videoMode.format = newFormat;
		_transactionDetails.formatChanged = true;
		_screenFormat = newFormat;
	}
#endif

	// Avoid redundant res changes
	if ((int)width == _videoMode.screenWidth && (int)height == _videoMode.screenHeight)
		return;

	_videoMode.screenWidth = width;
	_videoMode.screenHeight = height;

	_transactionDetails.sizeChanged = true;
}

int OpenGLGraphicsManager::getScreenChangeID() const {
	return _screenChangeCount;
}

//
// GFX
//

void OpenGLGraphicsManager::beginGFXTransaction() {
	assert(_transactionMode == kTransactionNone);

	_transactionMode = kTransactionActive;
	_transactionDetails.sizeChanged = false;
	_transactionDetails.needRefresh = false;
	_transactionDetails.needUpdatescreen = false;
	_transactionDetails.filterChanged = false;
#ifdef USE_RGB_COLOR
	_transactionDetails.formatChanged = false;
#endif

	_oldVideoMode = _videoMode;
}

OSystem::TransactionError OpenGLGraphicsManager::endGFXTransaction() {
	int errors = OSystem::kTransactionSuccess;

	assert(_transactionMode != kTransactionNone);

	if (_transactionMode == kTransactionRollback) {
		if (_videoMode.fullscreen != _oldVideoMode.fullscreen) {
			errors |= OSystem::kTransactionFullscreenFailed;

			_videoMode.fullscreen = _oldVideoMode.fullscreen;
		} else if (_videoMode.mode != _oldVideoMode.mode) {
			errors |= OSystem::kTransactionModeSwitchFailed;

			_videoMode.mode = _oldVideoMode.mode;
			_videoMode.scaleFactor = _oldVideoMode.scaleFactor;
#ifdef USE_RGB_COLOR
		} else if (_videoMode.format != _oldVideoMode.format) {
			errors |= OSystem::kTransactionFormatNotSupported;

			_videoMode.format = _oldVideoMode.format;
			_screenFormat = _videoMode.format;
#endif
		} else if (_videoMode.screenWidth != _oldVideoMode.screenWidth || _videoMode.screenHeight != _oldVideoMode.screenHeight) {
			errors |= OSystem::kTransactionSizeChangeFailed;

			_videoMode.screenWidth = _oldVideoMode.screenWidth;
			_videoMode.screenHeight = _oldVideoMode.screenHeight;
			_videoMode.overlayWidth = _oldVideoMode.overlayWidth;
			_videoMode.overlayHeight = _oldVideoMode.overlayHeight;
		}

		if (_videoMode.fullscreen == _oldVideoMode.fullscreen &&
		        _videoMode.mode == _oldVideoMode.mode &&
		        _videoMode.screenWidth == _oldVideoMode.screenWidth &&
		        _videoMode.screenHeight == _oldVideoMode.screenHeight) {

			_oldVideoMode.setup = false;
		}
	}

	if (_transactionDetails.sizeChanged || _transactionDetails.needRefresh) {
		unloadGFXMode();
		if (!loadGFXMode()) {
			if (_oldVideoMode.setup) {
				_transactionMode = kTransactionRollback;
				errors |= endGFXTransaction();
			}
		} else {
			clearOverlay();

			_videoMode.setup = true;
			_screenChangeCount++;
		}
#ifdef USE_RGB_COLOR
	} else if (_transactionDetails.filterChanged || _transactionDetails.formatChanged) {
#else
	} else if (_transactionDetails.filterChanged) {
#endif
		loadTextures();
		internUpdateScreen();
	} else if (_transactionDetails.needUpdatescreen) {
		internUpdateScreen();
	}

	_transactionMode = kTransactionNone;
	return (OSystem::TransactionError)errors;
}

//
// Screen
//

int16 OpenGLGraphicsManager::getHeight() {
	return _videoMode.screenHeight;
}

int16 OpenGLGraphicsManager::getWidth() {
	return _videoMode.screenWidth;
}

void OpenGLGraphicsManager::setPalette(const byte *colors, uint start, uint num) {
	assert(colors);

#ifdef USE_RGB_COLOR
	assert(_screenFormat.bytesPerPixel == 1);
#endif

	// Save the screen palette
	memcpy(_gamePalette + start * 3, colors, num * 3);

	_screenNeedsRedraw = true;

	if (_cursorPaletteDisabled)
		_cursorNeedsRedraw = true;
}

void OpenGLGraphicsManager::grabPalette(byte *colors, uint start, uint num) {
	assert(colors);

#ifdef USE_RGB_COLOR
	assert(_screenFormat.bytesPerPixel == 1);
#endif

	// Copies current palette to buffer
	memcpy(colors, _gamePalette + start * 3, num * 3);
}

void OpenGLGraphicsManager::copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) {
	assert(x >= 0 && x < _screenData.w);
	assert(y >= 0 && y < _screenData.h);
	assert(h > 0 && y + h <= _screenData.h);
	assert(w > 0 && x + w <= _screenData.w);

	// Copy buffer data to game screen internal buffer
	const byte *src = (const byte *)buf;
	byte *dst = (byte *)_screenData.pixels + y * _screenData.pitch + x * _screenData.format.bytesPerPixel;
	for (int i = 0; i < h; i++) {
		memcpy(dst, src, w * _screenData.format.bytesPerPixel);
		src += pitch;
		dst += _screenData.pitch;
	}

	// Extend dirty area if not full screen redraw is flagged
	if (!_screenNeedsRedraw) {
		const Common::Rect dirtyRect(x, y, x + w, y + h);
		_screenDirtyRect.extend(dirtyRect);
	}
}

Graphics::Surface *OpenGLGraphicsManager::lockScreen() {
	return &_screenData;
}

void OpenGLGraphicsManager::unlockScreen() {
	_screenNeedsRedraw = true;
}

void OpenGLGraphicsManager::fillScreen(uint32 col) {
	if (_gameTexture == NULL)
		return;

#ifdef USE_RGB_COLOR
	if (_screenFormat.bytesPerPixel == 1) {
		memset(_screenData.pixels, col, _screenData.h * _screenData.pitch);
	} else if (_screenFormat.bytesPerPixel == 2) {
		uint16 *pixels = (uint16 *)_screenData.pixels;
		uint16 col16 = (uint16)col;
		for (int i = 0; i < _screenData.w * _screenData.h; i++) {
			pixels[i] = col16;
		}
	} else if (_screenFormat.bytesPerPixel == 3) {
		uint8 *pixels = (uint8 *)_screenData.pixels;
		byte r = (col >> 16) & 0xFF;
		byte g = (col >> 8) & 0xFF;
		byte b = col & 0xFF;
		for (int i = 0; i < _screenData.w * _screenData.h; i++) {
			pixels[0] = r;
			pixels[1] = g;
			pixels[2] = b;
			pixels += 3;
		}
	} else if (_screenFormat.bytesPerPixel == 4) {
		uint32 *pixels = (uint32 *)_screenData.pixels;
		for (int i = 0; i < _screenData.w * _screenData.h; i++) {
			pixels[i] = col;
		}
	}
#else
	memset(_screenData.pixels, col, _screenData.h * _screenData.pitch);
#endif
	_screenNeedsRedraw = true;
}

void OpenGLGraphicsManager::updateScreen() {
	assert(_transactionMode == kTransactionNone);
	internUpdateScreen();
}

void OpenGLGraphicsManager::setShakePos(int shakeOffset) {
	assert(_transactionMode == kTransactionNone);
	_shakePos = shakeOffset;
}

void OpenGLGraphicsManager::setFocusRectangle(const Common::Rect &rect) {
}

void OpenGLGraphicsManager::clearFocusRectangle() {
}

//
// Overlay
//

void OpenGLGraphicsManager::showOverlay() {
	assert(_transactionMode == kTransactionNone);

	if (_overlayVisible)
		return;

	_overlayVisible = true;

	clearOverlay();
}

void OpenGLGraphicsManager::hideOverlay() {
	assert(_transactionMode == kTransactionNone);

	if (!_overlayVisible)
		return;

	_overlayVisible = false;

	clearOverlay();
}

Graphics::PixelFormat OpenGLGraphicsManager::getOverlayFormat() const {
	return _overlayFormat;
}

void OpenGLGraphicsManager::clearOverlay() {
	// Set all pixels to 0
	memset(_overlayData.pixels, 0, _overlayData.h * _overlayData.pitch);
	_overlayNeedsRedraw = true;
}

void OpenGLGraphicsManager::grabOverlay(void *buf, int pitch) {
	const byte *src = (byte *)_overlayData.pixels;
	byte *dst = (byte *)buf;
	for (int i = 0; i < _overlayData.h; i++) {
		// Copy overlay data to buffer
		memcpy(dst, src, _overlayData.pitch);
		dst += pitch;
		src += _overlayData.pitch;
	}
}

void OpenGLGraphicsManager::copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) {
	assert(_transactionMode == kTransactionNone);

	if (_overlayTexture == NULL)
		return;

	const byte *src = (const byte *)buf;

	// Clip the coordinates
	if (x < 0) {
		w += x;
		src -= x * 2;
		x = 0;
	}

	if (y < 0) {
		h += y;
		src -= y * pitch;
		y = 0;
	}

	if (w > _overlayData.w - x)
		w = _overlayData.w - x;

	if (h > _overlayData.h - y)
		h = _overlayData.h - y;

	if (w <= 0 || h <= 0)
		return;

	// Copy buffer data to internal overlay surface
	byte *dst = (byte *)_overlayData.pixels + y * _overlayData.pitch;
	for (int i = 0; i < h; i++) {
		memcpy(dst + x * _overlayData.format.bytesPerPixel, src, w * _overlayData.format.bytesPerPixel);
		src += pitch;
		dst += _overlayData.pitch;
	}

	// Extend dirty area if not full screen redraw is flagged
	if (!_overlayNeedsRedraw) {
		const Common::Rect dirtyRect(x, y, x + w, y + h);
		_overlayDirtyRect.extend(dirtyRect);
	}
}

int16 OpenGLGraphicsManager::getOverlayHeight() {
	return _videoMode.overlayHeight;
}

int16 OpenGLGraphicsManager::getOverlayWidth() {
	return _videoMode.overlayWidth;
}

//
// Cursor
//

bool OpenGLGraphicsManager::showMouse(bool visible) {
	if (_cursorVisible == visible)
		return visible;

	bool last = _cursorVisible;
	_cursorVisible = visible;

	return last;
}

void OpenGLGraphicsManager::warpMouse(int x, int y) {
	int scaledX = x;
	int scaledY = y;

	int16 currentX = _cursorState.x;
	int16 currentY = _cursorState.y;

	adjustMousePosition(currentX, currentY);

	// Do not adjust the real screen position, when the current game / overlay
	// coordinates match the requested coordinates. This avoids a slight
	// movement which might occur otherwise when the mouse is at a subpixel
	// position.
	if (x == currentX && y == currentY)
		return;

	if (_scaleMode == OpenGL::GFX_NORMAL) {
		if (_videoMode.hardwareWidth != _videoMode.overlayWidth)
			scaledX = scaledX * _videoMode.hardwareWidth / _videoMode.overlayWidth;
		if (_videoMode.hardwareHeight != _videoMode.overlayHeight)
			scaledY = scaledY * _videoMode.hardwareHeight / _videoMode.overlayHeight;

		if (!_overlayVisible) {
			scaledX *= _videoMode.scaleFactor;
			scaledY *= _videoMode.scaleFactor;
		}
	} else {
		if (_overlayVisible) {
			if (_displayWidth != _videoMode.overlayWidth)
				scaledX = scaledX * _displayWidth / _videoMode.overlayWidth;
			if (_displayHeight != _videoMode.overlayHeight)
				scaledY = scaledY * _displayHeight / _videoMode.overlayHeight;
		} else {
			if (_displayWidth != _videoMode.screenWidth)
				scaledX = scaledX * _displayWidth / _videoMode.screenWidth;
			if (_displayHeight != _videoMode.screenHeight)
				scaledY = scaledY * _displayHeight / _videoMode.screenHeight;
		}

		scaledX += _displayX;
		scaledY += _displayY;
	}

	setMousePosition(scaledX, scaledY);
	setInternalMousePosition(scaledX, scaledY);
}

void OpenGLGraphicsManager::setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY, uint32 keycolor, bool dontScale, const Graphics::PixelFormat *format) {
#ifdef USE_RGB_COLOR
	if (format)
		_cursorFormat = *format;
	else
		_cursorFormat = Graphics::PixelFormat::createFormatCLUT8();
#else
	assert(keycolor <= 255);
	_cursorFormat = Graphics::PixelFormat::createFormatCLUT8();
#endif

	// Allocate space for cursor data
	if (_cursorData.w != w || _cursorData.h != h ||
	        _cursorData.format.bytesPerPixel != _cursorFormat.bytesPerPixel)
		_cursorData.create(w, h, _cursorFormat);

	// Save cursor data
	memcpy(_cursorData.pixels, buf, h * _cursorData.pitch);

	// Set cursor info
	_cursorState.w = w;
	_cursorState.h = h;
	_cursorState.hotX = hotspotX;
	_cursorState.hotY = hotspotY;
	_cursorKeyColor = keycolor;
	_cursorDontScale = dontScale;
	_cursorNeedsRedraw = true;

	refreshCursorScale();
}

void OpenGLGraphicsManager::setCursorPalette(const byte *colors, uint start, uint num) {
	assert(colors);

	// Save the cursor palette
	memcpy(_cursorPalette + start * 3, colors, num * 3);

	_cursorPaletteDisabled = false;
	_cursorNeedsRedraw = true;
}

//
// Misc
//

void OpenGLGraphicsManager::displayMessageOnOSD(const char *msg) {
	assert(_transactionMode == kTransactionNone);
	assert(msg);

#ifdef USE_OSD
	// Split the message into separate lines.
	_osdLines.clear();

	Common::StringTokenizer tokenizer(msg, "\n");
	while (!tokenizer.empty())
		_osdLines.push_back(tokenizer.nextToken());

	// Request update of the texture
	_requireOSDUpdate = true;

	// Init the OSD display parameters, and the fade out
	_osdAlpha = kOSDInitialAlpha;
	_osdFadeStartTime = g_system->getMillis() + kOSDFadeOutDelay;
#endif
}

#ifdef USE_OSD
void OpenGLGraphicsManager::displayModeChangedMsg() {
	const char *newModeName = getCurrentModeName();
	if (newModeName) {
		const int scaleFactor = getScale();

		Common::String osdMessage = Common::String::format(
			"%s: %s\n%d x %d -> %d x %d",
			_("Current display mode"),
			newModeName,
			_videoMode.screenWidth * scaleFactor,
			_videoMode.screenHeight * scaleFactor,
			_videoMode.hardwareWidth, _videoMode.hardwareHeight
			);
		displayMessageOnOSD(osdMessage.c_str());
	}
}

void OpenGLGraphicsManager::displayScaleModeChangedMsg() {
	Common::String osdMessage = Common::String::format(
		"%s: %s",
		"Current scale mode",
		s_scaleModeNames[_scaleMode]
		);
	displayMessageOnOSD(osdMessage.c_str());
}

void OpenGLGraphicsManager::displayScaleChangedMsg() {
	const int scaleFactor = getScale();
	Common::String osdMessage = Common::String::format(
		"%s: x%d\n%d x %d -> %d x %d",
		_("Current scale"),
		scaleFactor,
		_videoMode.screenWidth, _videoMode.screenHeight,
		_videoMode.overlayWidth, _videoMode.overlayHeight
		);
	displayMessageOnOSD(osdMessage.c_str());
}
#endif


//
// Intern
//

void OpenGLGraphicsManager::setFullscreenMode(bool enable) {
	assert(_transactionMode == kTransactionActive);

	if (_oldVideoMode.setup && _oldVideoMode.fullscreen == enable)
		return;

	if (_transactionMode == kTransactionActive) {
		_videoMode.fullscreen = enable;
		_transactionDetails.needRefresh = true;
	}
}

void OpenGLGraphicsManager::refreshGameScreen() {
	if (_screenNeedsRedraw)
		_screenDirtyRect = Common::Rect(0, 0, _screenData.w, _screenData.h);

	int x = _screenDirtyRect.left;
	int y = _screenDirtyRect.top;
	int w = _screenDirtyRect.width();
	int h = _screenDirtyRect.height();

	if (_screenData.format.bytesPerPixel == 1) {
		// Create a temporary RGB888 surface
		byte *surface = new byte[w * h * 3];

		// Convert the paletted buffer to RGB888
		const byte *src = (byte *)_screenData.pixels + y * _screenData.pitch;
		src += x * _screenData.format.bytesPerPixel;
		byte *dst = surface;
		for (int i = 0; i < h; i++) {
			for (int j = 0; j < w; j++) {
				dst[0] = _gamePalette[src[j] * 3];
				dst[1] = _gamePalette[src[j] * 3 + 1];
				dst[2] = _gamePalette[src[j] * 3 + 2];
				dst += 3;
			}
			src += _screenData.pitch;
		}

		// Update the texture
		_gameTexture->updateBuffer(surface, w * 3, x, y, w, h);

		// Free the temp surface
		delete[] surface;
	} else {
		// Update the texture
		_gameTexture->updateBuffer((byte *)_screenData.pixels + y * _screenData.pitch +
		                           x * _screenData.format.bytesPerPixel, _screenData.pitch, x, y, w, h);
	}

	_screenNeedsRedraw = false;
	_screenDirtyRect = Common::Rect();
}

void OpenGLGraphicsManager::refreshOverlay() {
	if (_overlayNeedsRedraw)
		_overlayDirtyRect = Common::Rect(0, 0, _overlayData.w, _overlayData.h);

	int x = _overlayDirtyRect.left;
	int y = _overlayDirtyRect.top;
	int w = _overlayDirtyRect.width();
	int h = _overlayDirtyRect.height();

	if (_overlayData.format.bytesPerPixel == 1) {
		// Create a temporary RGB888 surface
		byte *surface = new byte[w * h * 3];

		// Convert the paletted buffer to RGB888
		const byte *src = (byte *)_overlayData.pixels + y * _overlayData.pitch;
		src += x * _overlayData.format.bytesPerPixel;
		byte *dst = surface;
		for (int i = 0; i < h; i++) {
			for (int j = 0; j < w; j++) {
				dst[0] = _gamePalette[src[j] * 3];
				dst[1] = _gamePalette[src[j] * 3 + 1];
				dst[2] = _gamePalette[src[j] * 3 + 2];
				dst += 3;
			}
			src += _screenData.pitch;
		}

		// Update the texture
		_overlayTexture->updateBuffer(surface, w * 3, x, y, w, h);

		// Free the temp surface
		delete[] surface;
	} else {
		// Update the texture
		_overlayTexture->updateBuffer((byte *)_overlayData.pixels + y * _overlayData.pitch +
		                              x * _overlayData.format.bytesPerPixel, _overlayData.pitch, x, y, w, h);
	}

	_overlayNeedsRedraw = false;
	_overlayDirtyRect = Common::Rect();
}

void OpenGLGraphicsManager::refreshCursor() {
	_cursorNeedsRedraw = false;

	// Allocate a texture big enough for cursor
	_cursorTexture->allocBuffer(_cursorState.w, _cursorState.h);

	// Create a temporary RGBA8888 surface
	byte *surface = new byte[_cursorState.w * _cursorState.h * 4];
	memset(surface, 0, _cursorState.w * _cursorState.h * 4);

	byte *dst = surface;

	// Convert the paletted cursor to RGBA8888
	if (_cursorFormat.bytesPerPixel == 1) {
		// Select palette
		byte *palette;
		if (_cursorPaletteDisabled)
			palette = _gamePalette;
		else
			palette = _cursorPalette;

		// Convert the paletted cursor to RGBA8888
		const byte *src = (byte *)_cursorData.pixels;
		for (int i = 0; i < _cursorState.w * _cursorState.h; i++) {
			// Check for keycolor
			if (src[i] != _cursorKeyColor) {
				dst[0] = palette[src[i] * 3];
				dst[1] = palette[src[i] * 3 + 1];
				dst[2] = palette[src[i] * 3 + 2];
				dst[3] = 255;
			}
			dst += 4;
		}
	} else {
		const bool gotNoAlpha = (_cursorFormat.aLoss == 8);

		// Convert the RGB cursor to RGBA8888
		if (_cursorFormat.bytesPerPixel == 2) {
			const uint16 *src = (uint16 *)_cursorData.pixels;
			for (int i = 0; i < _cursorState.w * _cursorState.h; i++) {
				// Check for keycolor
				if (src[i] != _cursorKeyColor) {
					_cursorFormat.colorToARGB(src[i], dst[3], dst[0], dst[1], dst[2]);

					if (gotNoAlpha)
						dst[3] = 255;
				}
				dst += 4;
			}
		} else if (_cursorFormat.bytesPerPixel == 4) {
			const uint32 *src = (uint32 *)_cursorData.pixels;
			for (int i = 0; i < _cursorState.w * _cursorState.h; i++) {
				// Check for keycolor
				if (src[i] != _cursorKeyColor) {
					_cursorFormat.colorToARGB(src[i], dst[3], dst[0], dst[1], dst[2]);

					if (gotNoAlpha)
						dst[3] = 255;
				}
				dst += 4;
			}
		}
	}

	// Update the texture with new cursor
	_cursorTexture->updateBuffer(surface, _cursorState.w * 4, 0, 0, _cursorState.w, _cursorState.h);

	// Free the temp surface
	delete[] surface;
}

void OpenGLGraphicsManager::refreshCursorScale() {
	// Calculate the scale factors of the screen.
	// We also totally ignore the aspect of the overlay cursor, since aspect
	// ratio correction only applies to the game screen.
	// TODO: It might make sense to always ignore scaling of the mouse cursor
	// when the overlay is visible.
	uint screenScaleFactorX = _videoMode.hardwareWidth * 10000 / _videoMode.screenWidth;
	uint screenScaleFactorY = _videoMode.hardwareHeight * 10000 / _videoMode.screenHeight;

	// Ignore scaling when the cursor should not be scaled.
	if (_cursorDontScale) {
		screenScaleFactorX = 10000;
		screenScaleFactorY = 10000;
	}

	// Apply them (without any possible) aspect ratio correction to the
	// overlay.
	_cursorState.rW = (int16)(_cursorState.w * screenScaleFactorX / 10000);
	_cursorState.rH = (int16)(_cursorState.h * screenScaleFactorY / 10000);
	_cursorState.rHotX = (int16)(_cursorState.hotX * screenScaleFactorX / 10000);
	_cursorState.rHotY = (int16)(_cursorState.hotY * screenScaleFactorY / 10000);

	// Only apply scaling when it's desired.
	if (_cursorDontScale) {
		screenScaleFactorX = 10000;
		screenScaleFactorY = 10000;
	} else {
		// Make sure we properly scale the cursor according to the desired aspect.
		int width, height;
		calculateDisplaySize(width, height);
		screenScaleFactorX = (width * 10000 / _videoMode.screenWidth);
		screenScaleFactorY = (height * 10000 / _videoMode.screenHeight);
	}

	// Apply the scale cursor scaling for the game screen.
	_cursorState.vW = (int16)(_cursorState.w * screenScaleFactorX / 10000);
	_cursorState.vH = (int16)(_cursorState.h * screenScaleFactorY / 10000);
	_cursorState.vHotX = (int16)(_cursorState.hotX * screenScaleFactorX / 10000);
	_cursorState.vHotY = (int16)(_cursorState.hotY * screenScaleFactorY / 10000);
}

void OpenGLGraphicsManager::calculateDisplaySize(int &width, int &height) {
	if (_scaleMode == OpenGL::GFX_ORIGINAL) {
		width = _videoMode.screenWidth;
		height = _videoMode.screenHeight;
	} else {
		width = _videoMode.hardwareWidth;
		height = _videoMode.hardwareHeight;

		uint aspectRatio = (_videoMode.hardwareWidth * 10000 + 5000) / _videoMode.hardwareHeight;
		uint desiredAspectRatio = getAspectRatio();

		// Adjust one screen dimension for mantaining the aspect ratio
		if (aspectRatio < desiredAspectRatio)
			height = (width * 10000 + 5000) / desiredAspectRatio;
		else if (aspectRatio > desiredAspectRatio)
			width = (height * desiredAspectRatio + 5000) / 10000;
	}
}

void OpenGLGraphicsManager::refreshDisplaySize() {
	calculateDisplaySize(_displayWidth, _displayHeight);

	// Adjust x and y for centering the screen
	_displayX = (_videoMode.hardwareWidth - _displayWidth) / 2;
	_displayY = (_videoMode.hardwareHeight - _displayHeight) / 2;
}

void OpenGLGraphicsManager::getGLPixelFormat(Graphics::PixelFormat pixelFormat, byte &bpp, GLenum &intFormat, GLenum &glFormat, GLenum &gltype) {
	if (pixelFormat == Graphics::PixelFormat(4, 8, 8, 8, 8, 24, 16, 8, 0)) { // RGBA8888
		bpp = 4;
		intFormat = GL_RGBA;
		glFormat = GL_RGBA;
		gltype = GL_UNSIGNED_BYTE;
	} else if (pixelFormat == Graphics::PixelFormat(3, 8, 8, 8, 0, 16, 8, 0, 0)) { // RGB888
		bpp = 3;
		intFormat = GL_RGB;
		glFormat = GL_RGB;
		gltype = GL_UNSIGNED_BYTE;
	} else if (pixelFormat == Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0)) { // RGB565
		bpp = 2;
		intFormat = GL_RGB;
		glFormat = GL_RGB;
		gltype = GL_UNSIGNED_SHORT_5_6_5;
	} else if (pixelFormat == Graphics::PixelFormat(2, 5, 5, 5, 1, 11, 6, 1, 0)) { // RGB5551
		bpp = 2;
		intFormat = GL_RGBA;
		glFormat = GL_RGBA;
		gltype = GL_UNSIGNED_SHORT_5_5_5_1;
	} else if (pixelFormat == Graphics::PixelFormat(2, 5, 5, 5, 0, 10, 5, 0, 0)) { // RGB555
		bpp = 2;
		intFormat = GL_RGB;
		glFormat = GL_BGRA;
		gltype = GL_UNSIGNED_SHORT_1_5_5_5_REV;
	} else if (pixelFormat == Graphics::PixelFormat(2, 4, 4, 4, 4, 12, 8, 4, 0)) { // RGBA4444
		bpp = 2;
		intFormat = GL_RGBA;
		glFormat = GL_RGBA;
		gltype = GL_UNSIGNED_SHORT_4_4_4_4;
	} else if (pixelFormat.bytesPerPixel == 1) { // CLUT8
		// If uses a palette, create texture as RGB888. The pixel data will be converted
		// later.
		bpp = 3;
		intFormat = GL_RGB;
		glFormat = GL_RGB;
		gltype = GL_UNSIGNED_BYTE;
#ifndef USE_GLES
	} else if (pixelFormat == Graphics::PixelFormat(4, 8, 8, 8, 8, 16, 8, 0, 24)) { // ARGB8888
		bpp = 4;
		intFormat = GL_RGBA;
		glFormat = GL_BGRA;
		gltype = GL_UNSIGNED_INT_8_8_8_8_REV;
	} else if (pixelFormat == Graphics::PixelFormat(2, 4, 4, 4, 4, 8, 4, 0, 12)) { // ARGB4444
		bpp = 2;
		intFormat = GL_RGBA;
		glFormat = GL_BGRA;
		gltype = GL_UNSIGNED_SHORT_4_4_4_4_REV;
	} else if (pixelFormat == Graphics::PixelFormat(4, 8, 8, 8, 8, 0, 8, 16, 24)) { // ABGR8888
		bpp = 4;
		intFormat = GL_RGBA;
		glFormat = GL_RGBA;
		gltype = GL_UNSIGNED_INT_8_8_8_8_REV;
	} else if (pixelFormat == Graphics::PixelFormat(4, 8, 8, 8, 8, 8, 16, 24, 0)) { // BGRA8888
		bpp = 4;
		intFormat = GL_RGBA;
		glFormat = GL_BGRA;
		gltype = GL_UNSIGNED_BYTE;
	} else if (pixelFormat == Graphics::PixelFormat(3, 8, 8, 8, 0, 0, 8, 16, 0)) { // BGR888
		bpp = 3;
		intFormat = GL_RGB;
		glFormat = GL_BGR;
		gltype = GL_UNSIGNED_BYTE;
	} else if (pixelFormat == Graphics::PixelFormat(2, 5, 6, 5, 0, 0, 5, 11, 0)) { // BGR565
		bpp = 2;
		intFormat = GL_RGB;
		glFormat = GL_BGR;
		gltype = GL_UNSIGNED_SHORT_5_6_5;
	} else if (pixelFormat == Graphics::PixelFormat(2, 5, 5, 5, 1, 1, 6, 11, 0)) { // BGRA5551
		bpp = 2;
		intFormat = GL_RGBA;
		glFormat = GL_BGRA;
		gltype = GL_UNSIGNED_SHORT_5_5_5_1;
	} else if (pixelFormat == Graphics::PixelFormat(2, 4, 4, 4, 4, 0, 4, 8, 12)) { // ABGR4444
		bpp = 2;
		intFormat = GL_RGBA;
		glFormat = GL_RGBA;
		gltype = GL_UNSIGNED_SHORT_4_4_4_4_REV;
	} else if (pixelFormat == Graphics::PixelFormat(2, 4, 4, 4, 4, 4, 8, 12, 0)) { // BGRA4444
		bpp = 2;
		intFormat = GL_RGBA;
		glFormat = GL_BGRA;
		gltype = GL_UNSIGNED_SHORT_4_4_4_4;
#endif
	} else {
		error("OpenGLGraphicsManager: Pixel format not supported");
	}
}

void OpenGLGraphicsManager::internUpdateScreen() {
	// Clear the screen buffer
	glClear(GL_COLOR_BUFFER_BIT); CHECK_GL_ERROR();

	if (_screenNeedsRedraw || !_screenDirtyRect.isEmpty())
		// Refresh texture if dirty
		refreshGameScreen();

	int scaleFactor = _videoMode.hardwareHeight / _videoMode.screenHeight;

	glPushMatrix();

	// Adjust game screen shake position
	glTranslatef(0, _shakePos * scaleFactor, 0); CHECK_GL_ERROR();

	// Draw the game screen
	if (_enableShaders)
		drawTexture(_gameTexture, _displayX, _displayY, _displayWidth, _displayHeight, _currentShader);
	else
		drawTexture(_gameTexture, _displayX, _displayY, _displayWidth, _displayHeight);

	glPopMatrix();

	if (_overlayVisible) {
		if (_overlayNeedsRedraw || !_overlayDirtyRect.isEmpty())
			// Refresh texture if dirty
			refreshOverlay();

		// Draw the overlay
		drawTexture(_overlayTexture, 0, 0, _videoMode.overlayWidth, _videoMode.overlayHeight);
	}

	if (_cursorVisible) {
		if (_cursorNeedsRedraw)
			// Refresh texture if dirty
			refreshCursor();

		glPushMatrix();

		// Adjust mouse shake position, unless the overlay is visible
		glTranslatef(0, _overlayVisible ? 0 : _shakePos * scaleFactor, 0); CHECK_GL_ERROR();

		// Draw the cursor
		if (_overlayVisible)
			drawTexture(_cursorTexture, _cursorState.x - _cursorState.rHotX,
			                            _cursorState.y - _cursorState.rHotY, _cursorState.rW, _cursorState.rH);
		else
			drawTexture(_cursorTexture, _cursorState.x - _cursorState.vHotX,
			                            _cursorState.y - _cursorState.vHotY, _cursorState.vW, _cursorState.vH);

		glPopMatrix();
	}

#ifdef USE_OSD
	if (_osdAlpha > 0) {
		if (_requireOSDUpdate) {
			updateOSD();
			_requireOSDUpdate = false;
		}

		// Update alpha value
		const int diff = g_system->getMillis() - _osdFadeStartTime;
		if (diff > 0) {
			if (diff >= kOSDFadeOutDuration) {
				// Back to full transparency
				_osdAlpha = 0;
			} else {
				// Do a fade out
				_osdAlpha = kOSDInitialAlpha - diff * kOSDInitialAlpha / kOSDFadeOutDuration;
			}
		}
		// Set the osd transparency
		glColor4f(1.0f, 1.0f, 1.0f, _osdAlpha / 100.0f); CHECK_GL_ERROR();

		// Draw the osd texture
		drawTexture(_osdTexture, 0, 0, _videoMode.hardwareWidth, _videoMode.hardwareHeight);

		// Reset color
		glColor4f(1.0f, 1.0f, 1.0f, 1.0f); CHECK_GL_ERROR();
	}
#endif
}

void OpenGLGraphicsManager::initGL() {
	// Check available GL Extensions
	GLTexture::initGLExtensions();

	// Disable 3D properties
	glDisable(GL_CULL_FACE); CHECK_GL_ERROR();
	glDisable(GL_DEPTH_TEST); CHECK_GL_ERROR();
	glDisable(GL_LIGHTING); CHECK_GL_ERROR();
	glDisable(GL_FOG); CHECK_GL_ERROR();
	glDisable(GL_DITHER); CHECK_GL_ERROR();
	glShadeModel(GL_FLAT); CHECK_GL_ERROR();
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST); CHECK_GL_ERROR();

	// Setup alpha blend (For overlay and cursor)
	glEnable(GL_BLEND); CHECK_GL_ERROR();
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); CHECK_GL_ERROR();

	// Enable rendering with vertex and coord arrays
	glEnableClientState(GL_VERTEX_ARRAY); CHECK_GL_ERROR();
	glEnableClientState(GL_TEXTURE_COORD_ARRAY); CHECK_GL_ERROR();

	glEnable(GL_TEXTURE_2D); CHECK_GL_ERROR();

	// Setup the GL viewport
	glViewport(0, 0, _videoMode.hardwareWidth, _videoMode.hardwareHeight); CHECK_GL_ERROR();

	// Setup coordinates system
	glMatrixMode(GL_PROJECTION); CHECK_GL_ERROR();
	glLoadIdentity(); CHECK_GL_ERROR();
#ifdef USE_GLES
	glOrthof(0, _videoMode.hardwareWidth, _videoMode.hardwareHeight, 0, -1, 1); CHECK_GL_ERROR();
#else
	glOrtho(0, _videoMode.hardwareWidth, _videoMode.hardwareHeight, 0, -1, 1); CHECK_GL_ERROR();
#endif
	glMatrixMode(GL_MODELVIEW); CHECK_GL_ERROR();
	glLoadIdentity(); CHECK_GL_ERROR();
}

void OpenGLGraphicsManager::loadTextures() {
#ifdef USE_RGB_COLOR
	if (_transactionDetails.formatChanged && _gameTexture) {
		delete _gameTexture;
		_gameTexture = 0;
	}
#endif

	if (!_gameTexture) {
		byte bpp;
		GLenum intformat;
		GLenum format;
		GLenum type;
#ifdef USE_RGB_COLOR
		getGLPixelFormat(_screenFormat, bpp, intformat, format, type);
#else
		getGLPixelFormat(Graphics::PixelFormat::createFormatCLUT8(), bpp, intformat, format, type);
#endif
		_gameTexture = new GLTexture(bpp, intformat, format, type);
	} else
		_gameTexture->refresh();

	_overlayFormat = Graphics::PixelFormat(2, 5, 5, 5, 1, 11, 6, 1, 0);

	if (!_overlayTexture) {
		byte bpp;
		GLenum intformat;
		GLenum format;
		GLenum type;
		getGLPixelFormat(_overlayFormat, bpp, intformat, format, type);
		_overlayTexture = new GLTexture(bpp, intformat, format, type);
	} else
		_overlayTexture->refresh();

	if (!_cursorTexture)
		_cursorTexture = new GLTexture(4, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE);
	else
		_cursorTexture->refresh();

	GLint filter = _videoMode.antialiasing ? GL_LINEAR : GL_NEAREST;
	_gameTexture->setFilter(filter);
	_overlayTexture->setFilter(filter);
	_cursorTexture->setFilter(filter);

	// Allocate texture memory and finish refreshing
	_gameTexture->allocBuffer(_videoMode.screenWidth, _videoMode.screenHeight);
	_overlayTexture->allocBuffer(_videoMode.overlayWidth, _videoMode.overlayHeight);
	_cursorTexture->allocBuffer(_cursorState.w, _cursorState.h);

	if (
#ifdef USE_RGB_COLOR
	    _transactionDetails.formatChanged ||
#endif
	    _oldVideoMode.screenWidth != _videoMode.screenWidth ||
	    _oldVideoMode.screenHeight != _videoMode.screenHeight)
		_screenData.create(_videoMode.screenWidth, _videoMode.screenHeight,
#ifdef USE_RGB_COLOR
		                   _screenFormat
#else
		                   Graphics::PixelFormat::createFormatCLUT8()
#endif
		                  );


	if (_oldVideoMode.overlayWidth != _videoMode.overlayWidth ||
	        _oldVideoMode.overlayHeight != _videoMode.overlayHeight)
		_overlayData.create(_videoMode.overlayWidth, _videoMode.overlayHeight,
		                    _overlayFormat);

	_screenNeedsRedraw = true;
	_overlayNeedsRedraw = true;
	_cursorNeedsRedraw = true;

	// We need to setup a proper unpack alignment value here, else we will
	// get problems with the texture updates, in case the surface data is
	// not properly aligned.
	// It is noteworthy this assumes the OSD uses the same BPP as the overlay
	// and that the cursor works with any alignment setting.
	int newAlignment = Common::gcd(_gameTexture->getBytesPerPixel(), _overlayTexture->getBytesPerPixel());
	assert(newAlignment == 1 || newAlignment == 2 || newAlignment == 4);
	glPixelStorei(GL_UNPACK_ALIGNMENT, newAlignment);

	// We use a "pack" alignment (when reading from textures) to 4 here,
	// since the only place where we really use it is the BMP screenshot
	// code and that requires the same alignment too.
	glPixelStorei(GL_PACK_ALIGNMENT, 4);

#ifdef USE_OSD
	if (!_osdTexture)
		_osdTexture = new GLTexture(2, GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1);
	else
		_osdTexture->refresh();

	_osdTexture->allocBuffer(_videoMode.overlayWidth, _videoMode.overlayHeight);

	// Update the OSD in case it is used right now
	_requireOSDUpdate = true;
#endif
}
// This function performs multipass rendering using shaders.
// TODO: check if extensions are available, test OpenGL ES, clean/split function,
//       generate buffers on gfxmode init, use x and y (instead of implicit 0,0)
void OpenGLGraphicsManager::drawTexture(GLTexture *texture, GLshort x, GLshort y, GLshort w, GLshort h, const ShaderInfo *info) {
	float outputw, outputh, inputw, inputh, texw, texh,
		  origInputw, origInputh, origTexw, origTexh;
	origInputw = inputw = texture->getWidth();
	origInputh = inputh = texture->getHeight();
	origTexw = texw = texture->getTextureWidth();
	origTexh = texh = texture->getTextureHeight();
	bool implicitPass = false;
	GLuint currentTexture = texture->getName();
	GLuint origTexture = currentTexture;
	GLuint fbo, outputtex;
	for (uint i = 0; i < info->passes.size(); ++i) {
		bool lastPass = (i == info->passes.size() - 1);

		const ShaderPass &p = info->passes[i];

		switch (p.xScaleMethod) {
			case ShaderPass::kFixed:
				outputw = p.xScale;
				break;
			case ShaderPass::kInput:
				outputw = inputw * p.xScale;
				break;
			case ShaderPass::kOutput:
				outputw = w * p.xScale;
				break;
			case ShaderPass::kNotSet:
				outputw = inputw;
		}

		if (lastPass) {
			if (p.xScaleMethod == ShaderPass::kNotSet) {
				outputw = w;
			} else {
				implicitPass = true;
			}
		}

		switch (p.yScaleMethod) {
			case ShaderPass::kFixed:
				outputh = p.yScale;
				break;
			case ShaderPass::kInput:
				outputh = inputh * p.yScale;
				break;
			case ShaderPass::kOutput:
				outputh = h * p.yScale;
				break;
			case ShaderPass::kNotSet:
				outputh = inputh;
		}
		// ChecShaderPass::k if last pass
		if (lastPass) {
			if (p.yScaleMethod == ShaderPass::kNotSet) {
				outputh = h;
			} else {
				implicitPass = true;
			}
		}

		if (!lastPass || implicitPass) {
			glGenTextures(1, &outputtex);
			glBindTexture(GL_TEXTURE_2D, outputtex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, p.filter); CHECK_GL_ERROR();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, p.filter); CHECK_GL_ERROR();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); CHECK_GL_ERROR();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT); CHECK_GL_ERROR();
			glTexImage2D(
					GL_TEXTURE_2D, 0, GL_RGB,
					outputw, outputh,
					0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

			glGenFramebuffersEXT(1, &fbo);
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
			glFramebufferTexture2DEXT(
					GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, 
					GL_TEXTURE_2D, outputtex, 0);
			GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
			if (status != GL_FRAMEBUFFER_COMPLETE_EXT) {
				error("Framebuffer creation failed with %x", status);
			}
			glPushMatrix();
			glPushAttrib(GL_VIEWPORT_BIT|GL_ENABLE_BIT);
			glLoadIdentity();
			glClear(GL_COLOR_BUFFER_BIT);
			glViewport(0,0,outputw, outputh);
		}
		glDisable(GL_BLEND);

		//Set up current Texture
		glActiveTexture(GL_TEXTURE1); CHECK_GL_ERROR();
		glBindTexture(GL_TEXTURE_2D, currentTexture); CHECK_GL_ERROR();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, p.filter); CHECK_GL_ERROR();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, p.filter); CHECK_GL_ERROR();

		// Set up orig Texture
		glActiveTexture(GL_TEXTURE0); CHECK_GL_ERROR();
		glBindTexture(GL_TEXTURE_2D, origTexture); CHECK_GL_ERROR();

		glUseProgram(p.program);

		glUniform1i(p.textureLoc, 1);
		glUniform1i(p.frameCountLoc, _frameCount);
		glUniform2f(p.inputSizeLoc, inputw, inputh);
		glUniform2f(p.outputSizeLoc, outputw, outputh);
		glUniform2f(p.textureSizeLoc, texw, texh);

		// Use non-standard uniforms
		glUniform2f(p.origInputSizeLoc, origInputw, origInputh);
		glUniform2f(p.origTextureSizeLoc, origTexw, origTexh);
		glUniform1i(p.origTextureLoc, 0);

		const GLfloat vertices[] = {
			0, 0,
			outputw, 0,
			0, outputh,
			outputw, outputh
		};
		const GLfloat texCoords[] = {
			0, 0,
			inputw/texw, 0,
			0, inputh/texh,
			inputw/texw, inputh/texh,
		};
		glTexCoordPointer(2, GL_FLOAT, 0, texCoords); CHECK_GL_ERROR();
		glVertexPointer(2, GL_FLOAT, 0, vertices); CHECK_GL_ERROR();

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); CHECK_GL_ERROR();

		glUseProgram(0);

		inputw = outputw;
		inputh = outputh;
		texw = outputw;
		texh = outputh;
		if (i)
			glDeleteTextures(1, &currentTexture);
		if (!lastPass || implicitPass) {
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
			glDeleteFramebuffersEXT(1, &fbo);
			glPopMatrix();
			glPopAttrib();
		}
		currentTexture = outputtex;
	}
	if (implicitPass) {
		const GLshort vertices[] = {
			0, 0,
			w, 0,
			0, h,
			w, h
		};
		const GLfloat texCoords[] = {
			0, 0,
			inputw/texw, 0,
			0, inputh/texh,
			inputw/texw, inputh/texh,
		};
		glBindTexture(GL_TEXTURE_2D, currentTexture);
		glTexCoordPointer(2, GL_FLOAT, 0, texCoords); CHECK_GL_ERROR();
		glVertexPointer(2, GL_SHORT, 0, vertices); CHECK_GL_ERROR();
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); CHECK_GL_ERROR();
		glDeleteTextures(1, &currentTexture);
	}
	glEnable(GL_BLEND);

	_frameCount++;
}

void OpenGLGraphicsManager::drawTexture(GLTexture *texture, GLshort x, GLshort y, GLshort w, GLshort h) {
	// Select this OpenGL texture
	glBindTexture(GL_TEXTURE_2D, texture->getName()); CHECK_GL_ERROR();

	// Calculate the texture rect that will be drawn
	const GLfloat texWidth = texture->getDrawWidth();
	const GLfloat texHeight = texture->getDrawHeight();
	const GLfloat texcoords[] = {
		0, 0,
		texWidth, 0,
		0, texHeight,
		texWidth, texHeight,
	};
	glTexCoordPointer(2, GL_FLOAT, 0, texcoords); CHECK_GL_ERROR();

	// Calculate the screen rect where the texture will be drawn
	const GLshort vertices[] = {
		x,      y,
		x + w,  y,
		x,      y + h,
		x + w,  y + h,
	};
	glVertexPointer(2, GL_SHORT, 0, vertices); CHECK_GL_ERROR();

	// Draw the texture to the screen buffer
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); CHECK_GL_ERROR();
}

bool OpenGLGraphicsManager::loadGFXMode() {
	// Use all available space for overlay
	_videoMode.overlayWidth = _videoMode.hardwareWidth;
	_videoMode.overlayHeight = _videoMode.hardwareHeight;

	// Initialize OpenGL settings
	initGL();

	loadTextures();

	initShaders();

	refreshCursorScale();

	refreshDisplaySize();

	internUpdateScreen();

	return true;
}

void OpenGLGraphicsManager::unloadGFXMode() {

}

void OpenGLGraphicsManager::setScale(int newScale) {
	assert(_transactionMode == kTransactionActive);

	if (newScale == _videoMode.scaleFactor)
		return;

	_videoMode.scaleFactor = newScale;
	_transactionDetails.sizeChanged = true;
}

void OpenGLGraphicsManager::toggleAntialiasing() {
	assert(_transactionMode == kTransactionActive);

	_videoMode.antialiasing = !_videoMode.antialiasing;
	_transactionDetails.filterChanged = true;
}

uint OpenGLGraphicsManager::getAspectRatio() const {
	// In case we enable aspect ratio correction we force a 4/3 ratio.
	// But just for 320x200 and 640x400 games, since other games do not need
	// this.
	// TODO: This makes OpenGL Normal behave like OpenGL Conserve, when aspect
	// ratio correction is enabled, but it's better than the previous 4/3 mode
	// mess at least...
	if (_videoMode.aspectRatioCorrection
	        && ((_videoMode.screenWidth == 320 && _videoMode.screenHeight == 200)
	            || (_videoMode.screenWidth == 640 && _videoMode.screenHeight == 400)))
		return 13333;
	else if (_scaleMode == OpenGL::GFX_NORMAL)
		return _videoMode.hardwareWidth * 10000 / _videoMode.hardwareHeight;
	else
		return _videoMode.screenWidth * 10000 / _videoMode.screenHeight;
}

void OpenGLGraphicsManager::adjustMousePosition(int16 &x, int16 &y) {
	if (_overlayVisible)
		return;

	x -= _displayX;
	y -= _displayY;

	if (_displayWidth != _videoMode.screenWidth)
		x = x * _videoMode.screenWidth / _displayWidth;
	if (_displayHeight != _videoMode.screenHeight)
		y = y * _videoMode.screenHeight / _displayHeight;
}

bool OpenGLGraphicsManager::parseShader(const Common::String &filename, ShaderInfo &info) {
	// FIXME
	Common::AdvXMLParser shaderParser;
	if (!shaderParser.loadFile(filename)) {
		warning("Could not open file:%s", filename.c_str());
		return false;
	}

	warning("Parsing shader: %s", filename.c_str());
	Common::XMLTree *root, *shader = NULL, *t;

	root = shaderParser.parse();

	for (uint i = 0; i < root->children.size(); ++i) {
		if (root->children[i]->type == Common::XMLTree::kKey &&
				root->children[i]->text == "shader") {
			shader = root->children[i];
		}
	}
	if (!shader) {
		warning("No shader element");
		delete root;
		return false;
	}

	info.vertex = 0;

	for (uint i = 0; i < shader->children.size(); ++i) {
		if (shader->children[i]->type != Common::XMLTree::kKey)
			continue;
		t = shader->children[i];

		if (t->text == "vertex") {
			if (t->children[0]->type != Common::XMLTree::kText) {
				warning("Unexpected key");
				delete root;
				return false;
			}
			const Common::String &src = t->children[0]->text;
			info.vertex = compileShader(src, GL_VERTEX_SHADER);
		} else if (t->text == "fragment") {
			if (t->children[0]->type != Common::XMLTree::kText) {
				warning("Unexpected key");
				delete root;
				return false;
			}
			ShaderPass p;
			bool x = false, y = false;
			p.xScaleMethod = ShaderPass::kNotSet;
			p.yScaleMethod = ShaderPass::kNotSet;
			p.filter = GL_NEAREST;
			if (t->attrs.contains("filter")) {
				const Common::String &value = t->attrs["filter"];
				if (value == "nearest") {
					p.filter = GL_NEAREST;
				} else if (value == "linear") {
					p.filter = GL_LINEAR;
				} else {
					warning("filter must be linear or nearest");
					delete root;
					return false;
				}
			}
			if (t->attrs.contains("size")) {
				x = y = true;
				p.xScaleMethod = ShaderPass::kFixed;
				p.yScaleMethod = ShaderPass::kFixed;
				const Common::String &value = t->attrs["size"];
				sscanf(value.c_str(), "%f", &p.xScale);
				p.yScale = p.xScale;
			}
			if (t->attrs.contains("size_x")) {
				if (x) {
					warning("Conflicting attributes");
					delete root;
					return false;
				}
				x = true;
				p.xScaleMethod = ShaderPass::kFixed;
				const Common::String &value = t->attrs["size_x"];
				sscanf(value.c_str(), "%f", &p.xScale);
			}
			if (t->attrs.contains("size_y")) {
				if (y) {
					warning("Conflicting attributes");
					delete root;
					return false;
				}
				y = true;
				p.yScaleMethod = ShaderPass::kFixed;
				const Common::String &value = t->attrs["size_y"];
				sscanf(value.c_str(), "%f", &p.yScale);
			}
			if (t->attrs.contains("scale")) {
				if (x || y) {
					warning("Conflicting attributes");
					delete root;
					return false;
				}
				x = y = true;
				p.xScaleMethod = ShaderPass::kInput;
				p.yScaleMethod = ShaderPass::kInput;
				const Common::String &value = t->attrs["scale"];
				sscanf(value.c_str(), "%f", &p.xScale);
				p.yScale = p.xScale;
			}
			if (t->attrs.contains("scale_x")) {
				if (x) {
					warning("Conflicting attributes");
					delete root;
					return false;
				}
				x = true;
				p.xScaleMethod = ShaderPass::kInput;
				const Common::String &value = t->attrs["scale_x"];
				sscanf(value.c_str(), "%f", &p.xScale);
			}
			if (t->attrs.contains("scale_y")) {
				if (y) {
					warning("Conflicting attributes");
					delete root;
					return false;
				}
				y = true;
				p.yScaleMethod = ShaderPass::kInput;
				const Common::String &value = t->attrs["scale_y"];
				sscanf(value.c_str(), "%f", &p.yScale);
			}
			if (t->attrs.contains("outscale")) {
				if (x || y) {
					warning("Conflicting attributes");
					delete root;
					return false;
				}
				x = y = true;
				p.xScaleMethod = ShaderPass::kOutput;
				p.yScaleMethod = ShaderPass::kOutput;
				const Common::String &value = t->attrs["outscale"];
				sscanf(value.c_str(), "%f", &p.xScale);
				p.yScale = p.xScale;
			}
			if (t->attrs.contains("outscale_x")) {
				if (x) {
					warning("Conflicting attributes");
					delete root;
					return false;
				}
				x = true;
				p.xScaleMethod = ShaderPass::kOutput;
				const Common::String &value = t->attrs["outscale_x"];
				sscanf(value.c_str(), "%f", &p.xScale);
			}
			if (t->attrs.contains("outscale_y")) {
				if (y) {
					warning("Conflicting attributes");
					delete root;
					return false;
				}
				y = true;
				p.yScaleMethod = ShaderPass::kOutput;
				const Common::String &value = t->attrs["outscale_y"];
				sscanf(value.c_str(), "%f", &p.yScale);
			}
			const Common::String &src = t->children[0]->text;
			p.fragment = compileShader(src, GL_FRAGMENT_SHADER);
			info.passes.push_back(p);
		}

	}

	// The vertex shader may not have been compiled when the pass was parsed.
	// Link the programs now.
	for (uint j = 0; j < info.passes.size(); ++j) {
		ShaderPass &p = info.passes[j];
		p.program = linkShaders(info.vertex, p.fragment);
		p.textureLoc = glGetUniformLocation(p.program, "rubyTexture");
		p.inputSizeLoc = glGetUniformLocation(p.program, "rubyInputSize");
		p.outputSizeLoc = glGetUniformLocation(p.program, "rubyOutputSize");
		p.textureSizeLoc = glGetUniformLocation(p.program, "rubyTextureSize");
		p.frameCountLoc = glGetUniformLocation(p.program, "rubyFrameCount");

		// Non-standard but sometimes used
		p.origTextureLoc = glGetUniformLocation(p.program, "rubyOrigTexture");
		p.origTextureSizeLoc = glGetUniformLocation(p.program, "rubyOrigTextureSize");
		p.origInputSizeLoc = glGetUniformLocation(p.program, "rubyOrigInputSize");
	}

	delete root;

	return true;
}

const char *s_defaultVertex = 
"#version 110\n "
"uniform vec2 rubyTextureSize; "
"void main() { "
"  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; "
"  gl_TexCoord[0] = gl_MultiTexCoord0; "
"} ";

const char *s_defaultFragment =
"#version 110\n "
"uniform sampler2D rubyTexture; "
"uniform vec2 rubyTextureSize; "
"void main() { "
"  gl_FragColor = texture2DProj(rubyTexture, gl_TexCoord[0]); "
"} ";

void OpenGLGraphicsManager::initShaders() {
	if (_shadersInited) {
		_currentShader = &_shaders[_videoMode.mode];
		_frameCount = 0;
		return;
	}
	_shadersInited = true;
	const char * versionStr = (const char *)glGetString(GL_VERSION);
	Common::String version;
	int majorVersion, minorVersion;
	Common::StringTokenizer st(versionStr);
	// Version number is 3rd token in OpenGL ES
	// and 1st token in OpenGL.
#ifdef USE_GLES
	st.nextToken();
	st.nextToken();
#endif
	version = st.nextToken();
	sscanf(version.c_str(), "%d.%d", &majorVersion, &minorVersion);
	_enableShaders = majorVersion >= 2;

	if (!_enableShaders)
		return;

	ShaderInfo dInfo;
	ShaderPass p;

	// Initialize built-in shader
	dInfo.vertex = compileShader(s_defaultVertex, GL_VERTEX_SHADER);
	p.fragment = compileShader(s_defaultFragment, GL_FRAGMENT_SHADER);
	p.program = linkShaders(dInfo.vertex, p.fragment);
	dInfo.name = "default";
	p.filter = GL_NEAREST;
	p.textureLoc = glGetUniformLocation(p.program, "rubyTexture");
	p.inputSizeLoc = glGetUniformLocation(p.program, "rubyInputSize");
	p.outputSizeLoc = glGetUniformLocation(p.program, "rubyOutputSize");
	p.textureSizeLoc = glGetUniformLocation(p.program, "rubyTextureSize");
	p.origTextureSizeLoc = glGetUniformLocation(p.program, "rubyOrigTextureSize");
	p.origTextureLoc = glGetUniformLocation(p.program, "rubyOrigTexture");
	p.origInputSizeLoc = glGetUniformLocation(p.program, "rubyOrigInputSize");

	p.xScaleMethod = ShaderPass::kNotSet;
	p.yScaleMethod = ShaderPass::kNotSet;
	dInfo.passes.push_back(p);

	_shaders.push_back(dInfo);

	for (uint i = 1; i < s_supportedGraphicsModes->size() - 1; ++i) {
		ShaderInfo info;
		OSystem::GraphicsMode &gm = (*s_supportedGraphicsModes)[i];
		info.name = Common::String(gm.name);
		if (parseShader(info.name, info)) {
			warning("Successfully compiled %s", info.name.c_str());
			_shaders.push_back(info);
		} else {
			warning("%s is not a shader file", info.name.c_str());
		}
	}

	_defaultShader = &_shaders[0];
	_currentShader = &_shaders[_videoMode.mode];
	_frameCount = 0;
	//_currentShader = &_shaders[0];
}

GLuint OpenGLGraphicsManager::compileShader(const Common::String &src, GLenum type) {
	int size = src.size();
	const char * source = src.c_str();
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, &size);
	glCompileShader(shader);

	int status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		char * buffer;
		int length;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
		buffer = new char[length];
		glGetShaderInfoLog(shader, length, NULL, buffer);
		if (type == GL_VERTEX_SHADER)
			error("Vertex shader compilation failed:\n%s", buffer);
		else
			error("Fragment shader compilation failed:\n%s", buffer);
	}
	return shader;
}

GLuint OpenGLGraphicsManager::linkShaders(GLuint vertex, GLuint fragment) {
	GLuint program = glCreateProgram();
	if (vertex)
		glAttachShader(program, vertex);
	if (fragment)
		glAttachShader(program, fragment);
	glLinkProgram(program);
	int status;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status) {
		char * buffer;
		int length;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
		buffer = new char[length];
		glGetProgramInfoLog(program, length, NULL, buffer);
		error("Shader program link failed:\n%s", buffer);
	}
	return program;
}

bool OpenGLGraphicsManager::saveScreenshot(const char *filename) {
	int width = _videoMode.hardwareWidth;
	int height = _videoMode.hardwareHeight;

	// A line of a BMP image must have a size divisible by 4.
	// We calculate the padding bytes needed here.
	// Since we use a 3 byte per pixel mode, we can use width % 4 here, since
	// it is equal to 4 - (width * 3) % 4. (4 - (width * Bpp) % 4, is the
	// usual way of computing the padding bytes required).
	const int linePaddingSize = width % 4;
	const int lineSize = width * 3 + linePaddingSize;

	// Allocate memory for screenshot
	uint8 *pixels = new uint8[lineSize * height];

	// Get pixel data from OpenGL buffer
#ifdef USE_GLES
	glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels); CHECK_GL_ERROR();
#else
	if (_formatBGR) {
		glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, pixels); CHECK_GL_ERROR();
	} else {
		glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels); CHECK_GL_ERROR();
	}
#endif

	// Open file
	Common::DumpFile out;
	out.open(filename);

	// Write BMP header
	out.writeByte('B');
	out.writeByte('M');
	out.writeUint32LE(height * lineSize + 54);
	out.writeUint32LE(0);
	out.writeUint32LE(54);
	out.writeUint32LE(40);
	out.writeUint32LE(width);
	out.writeUint32LE(height);
	out.writeUint16LE(1);
	out.writeUint16LE(24);
	out.writeUint32LE(0);
	out.writeUint32LE(0);
	out.writeUint32LE(0);
	out.writeUint32LE(0);
	out.writeUint32LE(0);
	out.writeUint32LE(0);

	// Write pixel data to BMP
	out.write(pixels, lineSize * height);

	// Free allocated memory
	delete[] pixels;

	return true;
}

const char *OpenGLGraphicsManager::getCurrentModeName() {
	const char *modeName = 0;
	const OSystem::GraphicsMode *g = getSupportedGraphicsModes();
	while (g->name) {
		if (g->id == _videoMode.mode) {
			modeName = g->description;
			break;
		}
		g++;
	}
	return modeName;
}

#ifdef USE_OSD
const Graphics::Font *OpenGLGraphicsManager::getFontOSD() {
	return FontMan.getFontByUsage(Graphics::FontManager::kLocalizedFont);
}

void OpenGLGraphicsManager::updateOSD() {
	// The font we are going to use:
	const Graphics::Font *font = getFontOSD();

	if (_osdSurface.w != _osdTexture->getWidth() || _osdSurface.h != _osdTexture->getHeight())
		_osdSurface.create(_osdTexture->getWidth(), _osdTexture->getHeight(), _overlayFormat);
	else
		// Clear everything
		memset(_osdSurface.pixels, 0, _osdSurface.h * _osdSurface.pitch);

	// Determine a rect which would contain the message string (clipped to the
	// screen dimensions).
	const int vOffset = 6;
	const int lineSpacing = 1;
	const int lineHeight = font->getFontHeight() + 2 * lineSpacing;
	int width = 0;
	int height = lineHeight * _osdLines.size() + 2 * vOffset;
	for (uint i = 0; i < _osdLines.size(); i++) {
		width = MAX(width, font->getStringWidth(_osdLines[i]) + 14);
	}

	// Clip the rect
	if (width > _osdSurface.w)
		width = _osdSurface.w;
	if (height > _osdSurface.h)
		height = _osdSurface.h;

	int dstX = (_osdSurface.w - width) / 2;
	int dstY = (_osdSurface.h - height) / 2;

	// Draw a dark gray rect (R = 40, G = 40, B = 40)
	const uint16 color = 0x294B;
	_osdSurface.fillRect(Common::Rect(dstX, dstY, dstX + width, dstY + height), color);

	// Render the message, centered, and in white
	for (uint i = 0; i < _osdLines.size(); i++) {
		font->drawString(&_osdSurface, _osdLines[i],
		                 dstX, dstY + i * lineHeight + vOffset + lineSpacing, width,
		                 0xFFFF, Graphics::kTextAlignCenter);
	}

	// Update the texture
	_osdTexture->updateBuffer(_osdSurface.pixels, _osdSurface.pitch, 0, 0,
	                          _osdSurface.w, _osdSurface.h);
}
#endif

#endif
