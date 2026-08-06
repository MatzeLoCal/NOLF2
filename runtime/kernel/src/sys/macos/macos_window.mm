// ----------------------------------------------------------------------- //
//
// MODULE  : macos_window.mm
//
// PURPOSE : macOS (Cocoa + OpenGL) platform window for the Jupiter EX engine.
//           This is the native replacement for the Win32 window/message-loop
//           in kernel/src/sys/win/client.cpp.
//
//           OpenGL-first per the project plan: a legacy (2.1) GL context is
//           created so the renderer can start fixed-function (matching the
//           D3D9-era engine, and portable toward SGI later). The eventual
//           GL->Metal port replaces only the context/surface creation here.
//
//           Exposes a small C ABI (LTMacWin_*) that the render/display hookup
//           calls; keeps all Objective-C confined to this file.
//
// ----------------------------------------------------------------------- //

#import <Cocoa/Cocoa.h>
#include "macos_input.h"
#import <AppKit/AppKit.h>
#include <OpenGL/gl.h>

#include "ltmacwindow.h"   // the C ABI declared for the rest of the engine

// --------------------------------------------------------------------------
// The GL view: owns the NSOpenGLContext and forwards the engine's frame draw.
// --------------------------------------------------------------------------
@interface LTGLView : NSOpenGLView
@property (nonatomic, assign) LTMacWin_DrawFn drawFn;
@property (nonatomic, assign) void*           drawUser;
@end

@implementation LTGLView

- (void)prepareOpenGL {
    [super prepareOpenGL];
    GLint swap = 1;
    [[self openGLContext] setValues:&swap forParameter:NSOpenGLContextParameterSwapInterval];
}

- (void)drawRect:(NSRect)dirty {
    // The engine drives the GL context imperatively via the LTMacWin_* ABI
    // (the renderer's nr_SwapBuffers presents each frame). If a push-model draw
    // callback is set, honour it here; otherwise do NOTHING — a placeholder
    // clear+flush in this Cocoa display-cycle callback would race with, and
    // overwrite, the engine's just-presented frame (which is what put a grey
    // window on screen over the renderer's green clear).
    if (self.drawFn) {
        [[self openGLContext] makeCurrentContext];
        self.drawFn(self.drawUser);
        [[self openGLContext] flushBuffer];
    }
}

// FPS games want the view to take key input.
- (BOOL)acceptsFirstResponder { return YES; }
@end

// --------------------------------------------------------------------------
// Window state (single main window; the engine is single-window).
// --------------------------------------------------------------------------
namespace {
    NSWindow*  g_pWindow = nil;
    LTGLView*  g_pView   = nil;
    bool       g_bShouldClose = false;
}

@interface LTWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation LTWindowDelegate
- (BOOL)windowShouldClose:(id)sender { g_bShouldClose = true; return NO; }
@end

// --------------------------------------------------------------------------
// C ABI
// --------------------------------------------------------------------------
extern "C" {

bool LTMacWin_Create(const char* pTitle, int width, int height, bool fullscreen) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Fullscreen = a BORDERLESS window covering the whole screen, with the
        // menu bar and Dock hidden. This is the "exclusive fullscreen" the game
        // wants, without CGDisplayCapture: capturing a display is a far worse
        // failure mode (a crashed or timed-out run leaves the whole machine with
        // a captured display), and it buys nothing here -- the pointer is already
        // hidden and decoupled, and nothing else can be on top of a borderless
        // screen-sized window. It also avoids the Spaces transition animation
        // that -toggleFullScreen: would run while the GL context is coming up.
        NSScreen*  screen = [NSScreen mainScreen];
        NSRect     screenFrame = screen ? [screen frame] : NSMakeRect(0, 0, width, height);
        NSRect     frame = fullscreen ? screenFrame : NSMakeRect(0, 0, width, height);
        NSUInteger style = fullscreen
            ? NSWindowStyleMaskBorderless
            : (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
               NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable);

        g_pWindow = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
        [g_pWindow setTitle:[NSString stringWithUTF8String:(pTitle ? pTitle : "Lithtech")]];
        [g_pWindow setDelegate:[[LTWindowDelegate alloc] init]];
        if (fullscreen) {
            [g_pWindow setFrameOrigin:screenFrame.origin];
            // Deliberately NOT raised above NSMainMenuWindowLevel: a borderless
            // screen-sized window with no close button that also floats above
            // everything is very hard to escape if the game ever hangs. At
            // normal level it still covers the screen while frontmost, and
            // Cmd-Tab behaves.
            // Presentation options only take effect while we are frontmost, so
            // switching away restores the menu bar and Dock.
            [NSApp setPresentationOptions:(NSApplicationPresentationHideDock |
                                           NSApplicationPresentationHideMenuBar)];
        } else {
            [g_pWindow center];
        }

        // Legacy (2.1) profile -> fixed-function available (renderer starts here).
        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersionLegacy,
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAAccelerated,
            NSOpenGLPFAColorSize, 24,
            NSOpenGLPFAAlphaSize, 8,
            NSOpenGLPFADepthSize, 24,
            NSOpenGLPFAStencilSize, 8,
            0
        };
        NSOpenGLPixelFormat* pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!pf) return false;

        g_pView = [[LTGLView alloc] initWithFrame:frame pixelFormat:pf];
        // Retina backing doubles the pixel count in each axis -- affordable in a
        // window, expensive full screen (a 2x 1728pt display means a 3456x2234
        // drawable). LT_HIDPI=1 forces it back on if wanted; everything
        // downstream follows LTMacWin_GetSize, so either is consistent.
        BOOL bHiDPI = fullscreen ? (getenv("LT_HIDPI") != NULL) : YES;
        [g_pView setWantsBestResolutionOpenGLSurface:bHiDPI];
        [g_pWindow setContentView:g_pView];
        [g_pWindow makeFirstResponder:g_pView];

        [g_pWindow makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        return true;
    }
}

void LTMacWin_SetDrawCallback(LTMacWin_DrawFn fn, void* user) {
    if (g_pView) { g_pView.drawFn = fn; g_pView.drawUser = user; }
}

// --------------------------------------------------------------------------
// Mouse capture (pointer lock).
//
// Two separate states:
//   g_bWantCapture   - what the GAME asked for (cursor mode: gameplay vs menu)
//   g_bCaptureActive - what is actually applied right now
// They differ while the app is in the background: capture must be dropped then,
// or Cmd-Tabbing away would leave the user with a mouse trapped by a
// non-frontmost app. It is restored as soon as the app is frontmost again.
//
// CGAssociateMouseAndMouseCursorPosition(false) is the important half: it stops
// the cursor from tracking the mouse while still delivering movement deltas, so
// the pointer can't leave the window or stall against a screen edge (which is
// what makes look-around feel like it "sticks" without capture).
// --------------------------------------------------------------------------
static bool g_bWantCapture   = false;
static bool g_bCaptureActive = false;
// Cursor visibility is tracked separately from capture: the game hides the
// pointer in menus too (it draws its own cursor sprite) but only locks it in
// gameplay. Hidden = the game asked for CM_None, OR we are capturing.
static bool g_bGameWantsCursor  = true;    // engine cursor mode == CM_Hardware
static bool g_bCursorHidden     = false;   // what is applied right now

// ⚠️ CGDisplayHideCursor/CGDisplayShowCursor are a REFERENCE COUNT, not a
// boolean: N hides need N shows. So we cannot re-assert "hidden" every frame
// the way we re-assert the mouse association -- that would run the count up and
// a single show would never bring the pointer back. Track our own depth, only
// hide when the pointer is OBSERVED visible (which also self-corrects when
// AppKit re-shows it under us), and always unwind the count fully on show.
static int g_nCursorHideDepth = 0;

static void ltmac_SetCursorHidden(bool bHide) {
    if (bHide) {
        if (CGCursorIsVisible()) {
            CGDisplayHideCursor(kCGDirectMainDisplay);
            g_nCursorHideDepth++;
        }
    } else {
        while (g_nCursorHideDepth > 0) {
            CGDisplayShowCursor(kCGDirectMainDisplay);
            g_nCursorHideDepth--;
        }
    }
}

static void ltmac_ApplyCursorVisibility(void) {
    // Only ever hide while WE are frontmost -- CGDisplayHideCursor affects the
    // whole display, so hiding it while in the background would leave the user
    // with an invisible pointer in every other app.
    bool bHide = [NSApp isActive] && (g_bCaptureActive || !g_bGameWantsCursor);
    bool bChanged = (bHide != g_bCursorHidden);
    g_bCursorHidden = bHide;

    // CGDisplay*Cursor, NOT [NSCursor hide]: AppKit re-shows the NSCursor
    // whenever the pointer crosses a view's cursor rects, so an NSCursor hide
    // silently comes back as soon as the mouse moves. Called every pump (not
    // only on transitions) so a re-show by AppKit is corrected.
    ltmac_SetCursorHidden(bHide);

    if (bChanged && getenv("LT_TRACE_INPUT"))
        fprintf(stderr, "[in] hardware cursor %s\n", bHide ? "HIDDEN" : "SHOWN");
}

// The game's hooked window proc (see ltmacwindow.h). NULL until HookWindow runs.
static LTMacWndProc g_pMacWndProc = NULL;
LTMacWndProc* LTMacWin_GetWndProcSlot(void) {
    if (getenv("LT_TRACE_INPUT"))
        fprintf(stderr, "[in] game requested the window-proc slot (mouse -> game)\n");
    return &g_pMacWndProc;
}

// Win32 packs the cursor position into LPARAM as two signed 16-bit halves.
static inline long ltmac_MakeLParam(int x, int y) {
    return (long)((unsigned long)(x & 0xFFFF) | ((unsigned long)(y & 0xFFFF) << 16));
}

// Cocoa view coords are y-up from the bottom-left; Win32/the game want y-down
// from the top-left, in the same pixel space the engine renders in (the backing
// store, not points -- see LTMacWin_GetSize).
static void ltmac_PostMouseMessage(unsigned int uMsg, NSEvent* ev) {
    if (!g_pMacWndProc || !g_pView) return;

    NSPoint pt = [g_pView convertPoint:[ev locationInWindow] fromView:nil];
    NSRect  bounds = [g_pView bounds];
    NSRect  backing = [g_pView convertRectToBacking:bounds];
    CGFloat sx = (NSWidth(bounds)  > 0.0) ? NSWidth(backing)  / NSWidth(bounds)  : 1.0;
    CGFloat sy = (NSHeight(bounds) > 0.0) ? NSHeight(backing) / NSHeight(bounds) : 1.0;

    int x = (int)(pt.x * sx);
    int y = (int)((NSHeight(bounds) - pt.y) * sy);

    unsigned long wParam = 0;
    if ([NSEvent pressedMouseButtons] & 1) wParam |= 0x0001;   // MK_LBUTTON
    if ([NSEvent pressedMouseButtons] & 2) wParam |= 0x0002;   // MK_RBUTTON

    static bool s_bFirst = true;
    if (s_bFirst && getenv("LT_TRACE_INPUT")) {
        s_bFirst = false;
        fprintf(stderr, "[in] first mouse message to game: msg=0x%x at (%d,%d)\n", uMsg, x, y);
    }
    g_pMacWndProc((void*)g_pWindow, uMsg, wParam, ltmac_MakeLParam(x, y));
}

void LTMacWin_SetCursorVisible(bool bVisible) {
    g_bGameWantsCursor = bVisible;
    ltmac_ApplyCursorVisibility();
}

static void ltmac_WarpCursorToWindowCentre(void) {
    if (!g_pWindow) return;
    NSRect  frame  = [g_pWindow frame];
    NSPoint centre = NSMakePoint(NSMidX(frame), NSMidY(frame));
    // CGWarpMouseCursorPosition is in screen coords with Y flipped vs Cocoa.
    NSScreen *screen = [g_pWindow screen] ?: [NSScreen mainScreen];
    CGFloat screenH = screen ? NSHeight([screen frame]) : 0.0;
    CGWarpMouseCursorPosition(CGPointMake(centre.x, screenH - centre.y));
}

// --------------------------------------------------------------------------
// Pointer CONFINEMENT -- the macOS stand-in for Win32 ClipCursor.
//
// Capture (decoupling) and confinement are two different mechanisms, and Win32
// used BOTH: client.cpp re-centres the cursor every frame while
// g_CV_CursorCenter is set, and render.cpp ClipCursor()s it to the window.
// We only ported the first. That matters because the game DROPS the lock all
// the time during normal play -- CInterfaceMgr::UpdateCursorState calls
// UseCursor(..., bLockCursorToCenter=FALSE) for menus, screens and the
// objective popups (GS_POPUP), which fire constantly. Full screen that is
// harmless (the window is the whole screen), but in a window the pointer -- and
// the click -- lands on the desktop behind the game.
//
// macOS has no ClipCursor, so do it the only way available: notice that the
// pointer has strayed outside the window and warp it back to the edge. Confined
// to the window FRAME, not the content rect, so the title bar stays grabbable
// in windowed mode.
// --------------------------------------------------------------------------
static bool ltmac_MouseCaptureAllowed(void);

static void ltmac_ConfinePointerToWindow(void) {
    if (!g_pWindow || !ltmac_MouseCaptureAllowed()) return;
    // Only while WE are frontmost -- otherwise we would fight the user for the
    // pointer after Cmd-Tab. And never while decoupled: the pointer is already
    // pinned then, and warping it would inject bogus deltas into mouse-look.
    if (![NSApp isActive] || g_bCaptureActive) return;

    NSScreen *screen = [g_pWindow screen] ?: [NSScreen mainScreen];
    if (!screen) return;
    CGFloat screenH = NSHeight([screen frame]);

    NSRect  frame = [g_pWindow frame];
    NSPoint pt    = [NSEvent mouseLocation];      // screen coords, y-up

    // One pixel of inset, so the clamped position is unambiguously inside and
    // we do not re-trigger on the boundary every frame.
    CGFloat minX = NSMinX(frame) + 1.0, maxX = NSMaxX(frame) - 1.0;
    CGFloat minY = NSMinY(frame) + 1.0, maxY = NSMaxY(frame) - 1.0;
    if (maxX <= minX || maxY <= minY) return;

    CGFloat x = pt.x < minX ? minX : (pt.x > maxX ? maxX : pt.x);
    CGFloat y = pt.y < minY ? minY : (pt.y > maxY ? maxY : pt.y);
    if (x == pt.x && y == pt.y) return;           // already inside

    // CGWarpMouseCursorPosition is y-DOWN screen space, Cocoa is y-up.
    CGWarpMouseCursorPosition(CGPointMake(x, screenH - y));
}

// Safety net: decoupling the mouse is a SYSTEM-WIDE CoreGraphics state. If the
// process dies while captured (a timed-out test run, a crash) the user can be
// left with a mouse that moves nothing. Always restore on the way out.
static void ltmac_RestoreMouseOnExit(void) {
    CGAssociateMouseAndMouseCursorPosition(true);
    // Unwind the FULL hide depth, not one level -- see ltmac_SetCursorHidden.
    // Leaving the process with an outstanding hide is how a crashed run leaves
    // the user with no pointer at all.
    ltmac_SetCursorHidden(false);
    CGDisplayShowCursor(kCGDirectMainDisplay);
}

static void ltmac_ApplyCursorVisibility(void);

static bool ltmac_MouseCaptureAllowed(void) {
    // LT_NO_MOUSE_CAPTURE=1 -- for headless/automated runs, which activate their
    // window and would otherwise steal the pointer from whoever is at the machine.
    static int s_nAllowed = -1;
    if (s_nAllowed < 0)
        s_nAllowed = getenv("LT_NO_MOUSE_CAPTURE") ? 0 : 1;
    return s_nAllowed != 0;
}

static void ltmac_ApplyMouseCapture(bool bCapture) {
    if (bCapture && !ltmac_MouseCaptureAllowed())
        bCapture = false;

    bool bChanged = (bCapture != g_bCaptureActive);
    g_bCaptureActive = bCapture;

    if (bChanged && getenv("LT_TRACE_INPUT"))
        fprintf(stderr, "[in] mouse capture %s\n", bCapture ? "ON" : "OFF");

    static bool s_bExitHookInstalled = false;
    if (!s_bExitHookInstalled) {
        s_bExitHookInstalled = true;
        atexit(ltmac_RestoreMouseOnExit);
    }

    if (bCapture) {
        // ⚠️ RE-ASSERT EVERY PUMP, do not early-out on "already captured".
        // Mouse/cursor association is process-external CoreGraphics state: the
        // window server re-associates it on app deactivation, display
        // reconfiguration and secure-input transitions, none of which we are
        // told about. Caching "we already captured" therefore latches a lie and
        // the pointer silently comes back to life on the desktop. The call is
        // idempotent and costs nothing per frame. (Same lesson as
        // CLTCursor::SetCursorMode's early-out in §10.)
        CGAssociateMouseAndMouseCursorPosition(false);
        if (bChanged) {
            ltmac_WarpCursorToWindowCentre();
            // A warp injects a large bogus delta into the next mouse event;
            // drop it so the view doesn't snap on capture.
            LTMacInput_ClearMouseDelta();
        }
    } else if (bChanged) {
        CGAssociateMouseAndMouseCursorPosition(true);
    }
    ltmac_ApplyCursorVisibility();
}

void LTMacWin_SetMouseCapture(bool bCapture) {
    g_bWantCapture = bCapture;
    ltmac_ApplyMouseCapture(bCapture && [NSApp isActive]);
}

bool LTMacWin_IsMouseCaptured(void) { return g_bCaptureActive; }

// Pump pending Cocoa events without blocking (engine drives the frame loop).
void LTMacWin_PumpEvents(void) {
    LTMacInput_TickInjection();
    LTMacInput_TickHold();

    // Capture state is re-evaluated every pump from the engine's own cursor-lock
    // variable (CursorCenter), ANDed with app activation so Cmd-Tab always frees
    // the mouse. Polling beats an event hook here: the game sets CursorCenter
    // through a console string at each interface transition, and re-asserting an
    // idempotent state costs nothing.
    bool bWant = LTMacWin_EngineWantsCursorLock();
    if (bWant != g_bWantCapture && getenv("LT_TRACE_INPUT"))
        fprintf(stderr, "[in] engine wants cursor lock %s\n", bWant ? "ON" : "OFF");
    g_bWantCapture = bWant;

    // [NSApp isActive] gates BOTH capture and cursor hiding (see below), so it
    // is worth watching on its own -- if it never turns YES, neither ever
    // applies and the pointer stays live on the desktop.
    if (getenv("LT_TRACE_INPUT")) {
        static int s_nLastActive = -1;
        int nActive = [NSApp isActive] ? 1 : 0;
        if (nActive != s_nLastActive) {
            s_nLastActive = nActive;
            fprintf(stderr, "[in] NSApp isActive %s (keyWindow=%d)\n",
                    nActive ? "YES" : "NO", g_pWindow ? (int)[g_pWindow isKeyWindow] : -1);
        }
    }

    // LT_TRACE_CURSOR=1 -- a once-a-second pointer-state line, deliberately
    // SEPARATE from LT_TRACE_INPUT (which logs every key event and is unusable
    // during a human play session). This is the only way to tell what the
    // window server actually did with our requests, because CoreGraphics has no
    // getter for the mouse/cursor association: infer it from whether the global
    // pointer MOVES while we believe we have it decoupled. "mouse=" changing
    // between lines while want=1 active=1 means the decoupling did not take.
    if (getenv("LT_TRACE_CURSOR")) {
        static double s_fLastBeat = 0.0;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double fNow = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
        if (fNow - s_fLastBeat >= 1.0) {
            s_fLastBeat = fNow;
            NSPoint pt = [NSEvent mouseLocation];
            fprintf(stderr, "[cur] want=%d applied=%d allowed=%d active=%d key=%d "
                            "cgVisible=%d gameWantsCursor=%d mouse=(%.0f,%.0f)\n",
                    (int)g_bWantCapture, (int)g_bCaptureActive,
                    (int)ltmac_MouseCaptureAllowed(), (int)[NSApp isActive],
                    g_pWindow ? (int)[g_pWindow isKeyWindow] : -1,
                    (int)CGCursorIsVisible(), (int)g_bGameWantsCursor,
                    pt.x, pt.y);
        }
    }
    ltmac_ApplyMouseCapture(g_bWantCapture && [NSApp isActive]);
    ltmac_ApplyCursorVisibility();   // also follows activation
    ltmac_ConfinePointerToWindow();  // the ClipCursor half, for when the lock is off
    @autoreleasepool {
        NSEvent* ev;
        while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate distantPast]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES])) {
            // Mouse position/clicks additionally go to the game's hooked window
            // proc -- that is the only path NOLF2 has for its own cursor and for
            // clicking menu items (see ltmacwindow.h / windowsx.h).
            switch ([ev type]) {
                case NSEventTypeMouseMoved:
                case NSEventTypeLeftMouseDragged:
                case NSEventTypeRightMouseDragged:
                    ltmac_PostMouseMessage(0x0200 /*WM_MOUSEMOVE*/, ev);
                    break;
                case NSEventTypeLeftMouseDown:
                    ltmac_PostMouseMessage([ev clickCount] >= 2 ? 0x0203 /*DBLCLK*/
                                                               : 0x0201 /*WM_LBUTTONDOWN*/, ev);
                    break;
                case NSEventTypeLeftMouseUp:
                    ltmac_PostMouseMessage(0x0202 /*WM_LBUTTONUP*/, ev);
                    break;
                case NSEventTypeRightMouseDown:
                    ltmac_PostMouseMessage([ev clickCount] >= 2 ? 0x0206 /*DBLCLK*/
                                                               : 0x0204 /*WM_RBUTTONDOWN*/, ev);
                    break;
                case NSEventTypeRightMouseUp:
                    ltmac_PostMouseMessage(0x0205 /*WM_RBUTTONUP*/, ev);
                    break;
                case NSEventTypeKeyDown:
                    // ★ WM_CHAR -- the ONLY path by which TYPED TEXT reaches the
                    // game. On Win32, TranslateMessage turns WM_KEYDOWN into
                    // WM_CHAR and the game's HookedWindowProc cracks it into
                    // CGameClientShell::OnChar -> CInterfaceMgr::OnChar ->
                    // HandleChar (chat input, save-game names, message boxes).
                    // Nothing here played TranslateMessage's role, so menus
                    // navigated (OnKeyDown flows via the separate VK queue) but
                    // no text field could ever receive a character.
                    // Keys continue to LTMacInput_HandleEvent below unchanged --
                    // this is an ADDITIONAL message, exactly like Win32 where
                    // WM_KEYDOWN and WM_CHAR both arrive.
                    if (g_pMacWndProc) {
                        NSString* chars = [ev characters];
                        for (NSUInteger i = 0; i < [chars length]; ++i) {
                            unichar c = [chars characterAtIndex:i];
                            // Printable Latin-1 only: the game's OnChar takes a
                            // char and CInterfaceMgr ignores c < ' ' itself;
                            // function/arrow keys arrive as F700+ and must not
                            // be smuggled into a text field as garbage bytes.
                            //
                            // ⚠️⚠️ 0x7F (DEL) AND THE C1 RANGE 0x80-0x9F ARE
                            // CONTROL CODES, NOT PRINTABLE. This matters:
                            // macOS reports the BACKSPACE key's -[NSEvent
                            // characters] as U+007F, which sailed through the
                            // old `c >= ' '` test. The result was that every
                            // backspace deleted a character via VK_BACK and
                            // then IMMEDIATELY APPENDED an invisible DEL via
                            // WM_CHAR — the caret bounced straight back to the
                            // end and the field looked frozen (§52). Win32
                            // never hit this: TranslateMessage gives backspace
                            // as WM_CHAR 0x08, which CInterfaceMgr::OnChar
                            // discards as < ' '.
                            if ((c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF))
                                g_pMacWndProc((void*)g_pWindow, 0x0102 /*WM_CHAR*/,
                                              (unsigned long)c,
                                              [ev isARepeat] ? 1 : 0);
                        }
                    }
                    break;
                default: break;
            }

            // Capture keyboard/mouse first; input events are consumed here
            // rather than dispatched (AppKit would beep at unhandled keys).
            if (!LTMacInput_HandleEvent((void*)ev))
                [NSApp sendEvent:ev];
        }
    }
}

void LTMacWin_SwapBuffers(void) {
    if (g_pView) [[g_pView openGLContext] flushBuffer];
}

void LTMacWin_MakeCurrent(void) {
    if (g_pView) [[g_pView openGLContext] makeCurrentContext];
}

bool LTMacWin_ShouldClose(void) { return g_bShouldClose; }

void LTMacWin_RequestClose(void) { g_bShouldClose = true; }

void LTMacWin_GetSize(int* w, int* h) {
    if (g_pView) {
        // Return the drawable size in BACKING pixels, not points. The view has
        // setWantsBestResolutionOpenGLSurface:YES, so on a Retina (2x) display
        // the GL framebuffer is 2x the point size. glViewport/glReadPixels work
        // in pixels — returning points here would size them to a quarter of the
        // buffer (the classic "renders in the lower-left quarter" bug).
        // ...but ONLY when the view actually asked for a hi-res surface. With
        // wantsBestResolutionOpenGLSurface:NO the GL drawable is the POINT size,
        // while convertRectToBacking still reports 2x -- reporting that would
        // set a viewport twice the real buffer. Honour the flag so this stays
        // the single source of truth in both windowed and fullscreen modes.
        NSRect bounds = [g_pView bounds];
        NSRect b = [g_pView wantsBestResolutionOpenGLSurface]
                 ? [g_pView convertRectToBacking:bounds]
                 : bounds;
        if (w) *w = (int)b.size.width;
        if (h) *h = (int)b.size.height;
    }
}

void* LTMacWin_GetNSWindow(void) { return (__bridge void*)g_pWindow; }

void LTMacWin_Destroy(void) {
    @autoreleasepool {
        ltmac_ApplyMouseCapture(false);   // never leave the pointer decoupled
        if (g_pWindow) { [g_pWindow close]; g_pWindow = nil; }
        g_pView = nil;
    }
}

void LTMacWin_SetTitle(const char* pTitle) {
    if (!g_pWindow || !pTitle) return;
    // AppKit requires -setTitle: on the main thread. The engine presents (and
    // the FPS counter updates the title) from whatever thread is drawing —
    // during level loads that is the LoadingScreen worker thread, which would
    // otherwise throw an NSException and abort. Marshal to the main queue.
    NSString *nsTitle = [NSString stringWithUTF8String:pTitle];
    if ([NSThread isMainThread]) {
        [g_pWindow setTitle:nsTitle];
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_pWindow) [g_pWindow setTitle:nsTitle];
        });
    }
}

} // extern "C"
