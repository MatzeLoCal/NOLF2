// ----------------------------------------------------------------------- //
//
// MODULE  : macos_input.mm
//
// PURPOSE : NSEvent keyboard/mouse capture — see macos_input.h for the two
//           consumers (VK queue for the UI, DIK state for gameplay bindings).
//
// ----------------------------------------------------------------------- //

#import <Cocoa/Cocoa.h>
#include "macos_input.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

// --------------------------------------------------------------------------
// Key translation.
//
// macOS reports hardware-independent virtual key codes (Carbon kVK_*). Map
// each to the Windows VK_* the UI expects and the PC DIK scancode the
// autoexec.cfg bindings use ("##17" is DIK_W = forward, etc).
// --------------------------------------------------------------------------

#define MACKEY_MAX 128

typedef struct
{
	unsigned char m_nMac;    // kVK_*
	unsigned char m_nVK;     // Windows VK_*
	unsigned char m_nDIK;    // PC scancode (set 1)
} LTMacKeyMap;

static const LTMacKeyMap kKeyMap[] =
{
	// Letters — VK_A..VK_Z are just 'A'..'Z'.
	{0x00,'A',0x1E}, {0x0B,'B',0x30}, {0x08,'C',0x2E}, {0x02,'D',0x20},
	{0x0E,'E',0x12}, {0x03,'F',0x21}, {0x05,'G',0x22}, {0x04,'H',0x23},
	{0x22,'I',0x17}, {0x26,'J',0x24}, {0x28,'K',0x25}, {0x25,'L',0x26},
	{0x2E,'M',0x32}, {0x2D,'N',0x31}, {0x1F,'O',0x18}, {0x23,'P',0x19},
	{0x0C,'Q',0x10}, {0x0F,'R',0x13}, {0x01,'S',0x1F}, {0x11,'T',0x14},
	{0x20,'U',0x16}, {0x09,'V',0x2F}, {0x0D,'W',0x11}, {0x07,'X',0x2D},
	{0x10,'Y',0x15}, {0x06,'Z',0x2C},

	// Number row.
	{0x1D,'0',0x0B}, {0x12,'1',0x02}, {0x13,'2',0x03}, {0x14,'3',0x04},
	{0x15,'4',0x05}, {0x17,'5',0x06}, {0x16,'6',0x07}, {0x1A,'7',0x08},
	{0x1C,'8',0x09}, {0x19,'9',0x0A},

	// Editing / whitespace.
	{0x24,0x0D,0x1C},   // Return
	{0x30,0x09,0x0F},   // Tab
	{0x31,0x20,0x39},   // Space
	{0x33,0x08,0x0E},   // Backspace (macOS "Delete")
	{0x35,0x1B,0x01},   // Escape
	{0x75,0x2E,0xD3},   // Forward delete
	{0x73,0x24,0xC7},   // Home
	{0x77,0x23,0xCF},   // End
	{0x74,0x21,0xC9},   // Page Up
	{0x79,0x22,0xD1},   // Page Down

	// Arrows.
	{0x7B,0x25,0xCB}, {0x7E,0x26,0xC8}, {0x7C,0x27,0xCD}, {0x7D,0x28,0xD0},

	// Punctuation.
	{0x1B,0xBD,0x0C},   // -
	{0x18,0xBB,0x0D},   // =
	{0x21,0xDB,0x1A},   // [
	{0x1E,0xDD,0x1B},   // ]
	{0x2A,0xDC,0x2B},   // backslash
	{0x29,0xBA,0x27},   // ;
	{0x27,0xDE,0x28},   // '
	{0x32,0xC0,0x29},   // `
	{0x2B,0xBC,0x33},   // ,
	{0x2F,0xBE,0x34},   // .
	{0x2C,0xBF,0x35},   // /

	// Modifiers (see flagsChanged handling).
	{0x38,0x10,0x2A},   // Left Shift
	{0x3C,0x10,0x36},   // Right Shift
	{0x3B,0x11,0x1D},   // Left Control
	{0x3E,0x11,0x9D},   // Right Control
	{0x3A,0x12,0x38},   // Left Option -> Alt
	{0x3D,0x12,0xB8},   // Right Option -> Alt

	// Function keys.
	{0x7A,0x70,0x3B}, {0x78,0x71,0x3C}, {0x63,0x72,0x3D}, {0x76,0x73,0x3E},
	{0x60,0x74,0x3F}, {0x61,0x75,0x40}, {0x62,0x76,0x41}, {0x64,0x77,0x42},
	{0x65,0x78,0x43}, {0x6D,0x79,0x44}, {0x67,0x7A,0x57}, {0x6F,0x7B,0x58},

	// Keypad.
	{0x52,0x60,0x52}, {0x53,0x61,0x4F}, {0x54,0x62,0x50}, {0x55,0x63,0x51},
	{0x56,0x64,0x4B}, {0x57,0x65,0x4C}, {0x58,0x66,0x4D}, {0x59,0x67,0x47},
	{0x5B,0x68,0x48}, {0x5C,0x69,0x49},
	{0x4C,0x0D,0x9C},   // Keypad Enter
	{0x4E,0x6D,0x4A},   // Keypad -
	{0x45,0x6B,0x4E},   // Keypad +
	{0x43,0x6A,0x37},   // Keypad *
	{0x4B,0x6F,0xB5},   // Keypad /
	{0x41,0x6E,0x53},   // Keypad .
};

// Built once from kKeyMap for O(1) lookup by macOS keycode.
static unsigned char g_aMacToVK[MACKEY_MAX];
static unsigned char g_aMacToDIK[MACKEY_MAX];
static bool g_bTablesBuilt = false;

static void input_BuildTables(void)
{
	if (g_bTablesBuilt)
		return;
	memset(g_aMacToVK, 0, sizeof(g_aMacToVK));
	memset(g_aMacToDIK, 0, sizeof(g_aMacToDIK));
	for (size_t i = 0; i < sizeof(kKeyMap) / sizeof(kKeyMap[0]); ++i)
	{
		unsigned char nMac = kKeyMap[i].m_nMac;
		if (nMac < MACKEY_MAX)
		{
			g_aMacToVK[nMac]  = kKeyMap[i].m_nVK;
			g_aMacToDIK[nMac] = kKeyMap[i].m_nDIK;
		}
	}
	g_bTablesBuilt = true;
}

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------

#define MAX_QUEUED_KEYS 64
#define DIK_STATE_SIZE  256

typedef struct { unsigned int m_nVK; unsigned int m_nRepeat; } LTMacKeyEvent;

static LTMacKeyEvent g_aKeyDowns[MAX_QUEUED_KEYS];
static unsigned int  g_nKeyDowns = 0;
static unsigned int  g_aKeyUps[MAX_QUEUED_KEYS];
static unsigned int  g_nKeyUps = 0;

static bool  g_aDIKDown[DIK_STATE_SIZE];
static float g_fMouseDX = 0.0f, g_fMouseDY = 0.0f;
static bool  g_aMouseDown[3];

// Previous modifier flags, to synthesise key up/down from flagsChanged.
static NSUInteger g_nPrevFlags = 0;

static void input_PushKeyDown(unsigned int nVK, unsigned int nRepeat)
{
	if (!nVK || g_nKeyDowns >= MAX_QUEUED_KEYS)
		return;
	g_aKeyDowns[g_nKeyDowns].m_nVK = nVK;
	g_aKeyDowns[g_nKeyDowns].m_nRepeat = nRepeat;
	++g_nKeyDowns;
}

static void input_PushKeyUp(unsigned int nVK)
{
	if (!nVK || g_nKeyUps >= MAX_QUEUED_KEYS)
		return;
	g_aKeyUps[g_nKeyUps] = nVK;
	++g_nKeyUps;
}

// --------------------------------------------------------------------------
// Event handling
// --------------------------------------------------------------------------

bool LTMacInput_HandleEvent(void *pNSEvent)
{
	input_BuildTables();

	NSEvent *ev = (NSEvent*)pNSEvent;
	if (!ev)
		return false;

	switch ([ev type])
	{
		case NSEventTypeKeyDown:
		{
			unsigned short nMac = [ev keyCode];
			if (nMac >= MACKEY_MAX)
				return true;
			unsigned char nDIK = g_aMacToDIK[nMac];
			if (nDIK)
				g_aDIKDown[nDIK] = true;
			if (getenv("LT_TRACE_INPUT"))
				fprintf(stderr, "[in] EVENT keyDown mac=%u -> DIK %u / VK %u\n",
				        nMac, nDIK, g_aMacToVK[nMac]);
			input_PushKeyDown(g_aMacToVK[nMac], [ev isARepeat] ? 1 : 0);
			// Swallow: never let AppKit beep at unhandled key equivalents.
			return true;
		}

		case NSEventTypeKeyUp:
		{
			unsigned short nMac = [ev keyCode];
			if (nMac >= MACKEY_MAX)
				return true;
			unsigned char nDIK = g_aMacToDIK[nMac];
			if (nDIK)
				g_aDIKDown[nDIK] = false;
			if (getenv("LT_TRACE_INPUT"))
				fprintf(stderr, "[in] EVENT keyUp   mac=%u -> DIK %u / VK %u\n",
				        nMac, nDIK, g_aMacToVK[nMac]);
			input_PushKeyUp(g_aMacToVK[nMac]);
			return true;
		}

		case NSEventTypeFlagsChanged:
		{
			// Modifiers arrive as a flags snapshot, not up/down — diff it.
			unsigned short nMac = [ev keyCode];
			if (nMac >= MACKEY_MAX)
				return true;
			NSUInteger nFlags = [ev modifierFlags];
			NSUInteger nMask = 0;
			switch (nMac)
			{
				case 0x38: case 0x3C: nMask = NSEventModifierFlagShift;   break;
				case 0x3B: case 0x3E: nMask = NSEventModifierFlagControl; break;
				case 0x3A: case 0x3D: nMask = NSEventModifierFlagOption;  break;
				default: break;
			}
			if (nMask)
			{
				bool bDown = (nFlags & nMask) != 0;
				bool bWasDown = (g_nPrevFlags & nMask) != 0;
				if (bDown != bWasDown)
				{
					unsigned char nDIK = g_aMacToDIK[nMac];
					if (nDIK)
						g_aDIKDown[nDIK] = bDown;
					if (bDown) input_PushKeyDown(g_aMacToVK[nMac], 0);
					else       input_PushKeyUp(g_aMacToVK[nMac]);
				}
			}
			g_nPrevFlags = nFlags;
			return true;
		}

		case NSEventTypeMouseMoved:
		case NSEventTypeLeftMouseDragged:
		case NSEventTypeRightMouseDragged:
		case NSEventTypeOtherMouseDragged:
			// Relative motion — what the look/turn axes want.
			g_fMouseDX += (float)[ev deltaX];
			g_fMouseDY += (float)[ev deltaY];
			return true;

		case NSEventTypeLeftMouseDown:   g_aMouseDown[0] = true;  return true;
		case NSEventTypeLeftMouseUp:     g_aMouseDown[0] = false; return true;
		case NSEventTypeRightMouseDown:  g_aMouseDown[1] = true;  return true;
		case NSEventTypeRightMouseUp:    g_aMouseDown[1] = false; return true;
		case NSEventTypeOtherMouseDown:  g_aMouseDown[2] = true;  return true;
		case NSEventTypeOtherMouseUp:    g_aMouseDown[2] = false; return true;

		default:
			break;
	}
	return false;
}

// --------------------------------------------------------------------------
// Accessors
// --------------------------------------------------------------------------

unsigned int LTMacInput_NumKeyDowns(void)                { return g_nKeyDowns; }
unsigned int LTMacInput_GetKeyDown(unsigned int i)       { return (i < g_nKeyDowns) ? g_aKeyDowns[i].m_nVK : 0; }
unsigned int LTMacInput_GetKeyDownRep(unsigned int i)    { return (i < g_nKeyDowns) ? g_aKeyDowns[i].m_nRepeat : 0; }
unsigned int LTMacInput_NumKeyUps(void)                  { return g_nKeyUps; }
unsigned int LTMacInput_GetKeyUp(unsigned int i)         { return (i < g_nKeyUps) ? g_aKeyUps[i] : 0; }
void         LTMacInput_ClearKeyDowns(void)              { g_nKeyDowns = 0; }
void         LTMacInput_ClearKeyUps(void)                { g_nKeyUps = 0; }

bool LTMacInput_IsDIKDown(unsigned int nDIK)
{
	return (nDIK < DIK_STATE_SIZE) ? g_aDIKDown[nDIK] : false;
}

void LTMacInput_ConsumeMouseDelta(float *pDX, float *pDY)
{
	if (pDX) *pDX = g_fMouseDX;
	if (pDY) *pDY = g_fMouseDY;
	g_fMouseDX = g_fMouseDY = 0.0f;
}

void LTMacInput_ClearMouseDelta(void)
{
	g_fMouseDX = g_fMouseDY = 0.0f;
}

bool LTMacInput_IsMouseButtonDown(unsigned int nButton)
{
	return (nButton < 3) ? g_aMouseDown[nButton] : false;
}

// --------------------------------------------------------------------------
// Headless test injection.
//
// LT_TEST_KEYS="38 38 13"  — a space-separated list of Windows VK codes fed
// into the queue as real keystrokes, one per LT_TEST_KEYS_INTERVAL frames
// (default 30) starting at LT_TEST_KEYS_START (default 1500 — after the splash
// has faded and the menu is up). Lets the menu be driven from a timed,
// windowless run: verifying "input works" otherwise needs a human at the
// keyboard. Each entry emits a down and an up.
// --------------------------------------------------------------------------
void LTMacInput_TickInjection(void)
{
	static bool s_bParsed = false;
	static unsigned int s_aKeys[32];
	static unsigned int s_nKeys = 0;
	// Time-based: the pump's call rate isn't 1:1 with presented frames, and the
	// splash takes a fixed ~20s of wall clock regardless of frame rate.
	static double s_fStart = 25.0, s_fInterval = 1.0;
	static double s_fT0 = 0.0;
	static unsigned int s_nNext = 0;

	if (!s_bParsed)
	{
		s_bParsed = true;
		const char *pEnv = getenv("LT_TEST_KEYS");
		if (pEnv)
		{
			const char *p = pEnv;
			while (*p && s_nKeys < 32)
			{
				while (*p == ' ') ++p;
				if (!*p) break;
				s_aKeys[s_nKeys++] = (unsigned int)strtoul(p, (char**)&p, 10);
			}
			if (const char *pS = getenv("LT_TEST_KEYS_START"))    s_fStart = atof(pS);
			if (const char *pI = getenv("LT_TEST_KEYS_INTERVAL")) s_fInterval = atof(pI);
			if (s_fInterval <= 0.0) s_fInterval = 1.0;
			fprintf(stderr, "[in] %u test keys, start %.1fs, every %.1fs\n",
			        s_nKeys, s_fStart, s_fInterval);
		}
	}

	if (s_nNext >= s_nKeys)
		return;

	struct timeval tv;
	gettimeofday(&tv, NULL);
	double fNow = (double)tv.tv_sec + tv.tv_usec * 1e-6;
	if (s_fT0 == 0.0) { s_fT0 = fNow; return; }

	double fElapsed = fNow - s_fT0;
	if (fElapsed < s_fStart + s_fInterval * (double)s_nNext)
		return;

	unsigned int nVK = s_aKeys[s_nNext++];
	input_PushKeyDown(nVK, 0);
	input_PushKeyUp(nVK);
	fprintf(stderr, "[in] injected VK %u (t=%.1fs)\n", nVK, fElapsed);
}

// LT_TEST_HOLD_DIK="17" — hold a DIK scancode down from LT_TEST_HOLD_START
// seconds (default 5). Exercises the gameplay path (InputMgr::ReadInput ->
// bindings -> actions) without a human holding the key.
void LTMacInput_TickHold(void)
{
	static int s_nDIK = -1;
	static double s_fStart = 5.0, s_fT0 = 0.0;
	if (s_nDIK < 0)
	{
		const char *pEnv = getenv("LT_TEST_HOLD_DIK");
		s_nDIK = pEnv ? atoi(pEnv) : 0;
		if (const char *pS = getenv("LT_TEST_HOLD_START")) s_fStart = atof(pS);
		if (s_nDIK)
			fprintf(stderr, "[in] holding DIK %d from %.1fs\n", s_nDIK, s_fStart);
	}
	if (!s_nDIK)
		return;

	struct timeval tv;
	gettimeofday(&tv, NULL);
	double fNow = (double)tv.tv_sec + tv.tv_usec * 1e-6;
	if (s_fT0 == 0.0) { s_fT0 = fNow; return; }
	if ((fNow - s_fT0) >= s_fStart && s_nDIK < DIK_STATE_SIZE)
		g_aDIKDown[s_nDIK] = true;
}

void LTMacInput_ClearAll(void)
{
	memset(g_aDIKDown, 0, sizeof(g_aDIKDown));
	memset(g_aMouseDown, 0, sizeof(g_aMouseDown));
	g_nKeyDowns = g_nKeyUps = 0;
	g_fMouseDX = g_fMouseDY = 0.0f;
}
