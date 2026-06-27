// ---------------------------------------------------------------------------
// pacman.cpp — Pac-Man chase animation for 16x16 NeoPixel matrix
//
// Lane system:
//   Horizontal top    (y=0-7):  sprites at row=0, travel left/right
//   Horizontal bottom (y=8-15): sprites at row=8, travel left/right
//   Vertical left     (x=0-7):  sprites at col=0, travel up/down
//   Vertical right    (x=8-15): sprites at col=8, travel up/down
//
// Sprites are NEVER rotated or mirrored — drawn exactly as in the sheet.
// Pac-Man uses the correct directional animation for his current heading.
// Static bonus item sits at a random mid-lane position; score floats upward.
// ---------------------------------------------------------------------------

#include "pacman.h"
#include "packman6_sprites8x8.h"
#include "../sprites/sprite_render.h"
#include "led_display.h"
#include <FastLED.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <Arduino.h>

// ---------------------------------------------------------------------------
// SpriteInfo
// ---------------------------------------------------------------------------
struct SpriteInfo {
    const char* name;
    uint8_t     rightStart;   // right-facing (or only) start frame
    uint8_t     leftStart;    // left-facing start frame
    uint8_t     frameCount;
    const char* scoreText;
};

// ---------------------------------------------------------------------------
// <<< GENERATED: HUNTER_SPRITES >>>
// SpriteInfo: name, rightStart, leftStart, frameCount, scoreText
static const SpriteInfo HUNTER_SPRITES[] = {
    { "red_ghost", 26, 28, 2, "200" },  // right=26-27,left=28-29
    { "pink_ghost", 30, 32, 2, "200" },  // right=30-31,left=32-33
    { "ltblue_ghost", 34, 36, 2, "200" },  // right=34-35,left=36-37
    { "orange_ghost", 38, 40, 2, "200" },  // right=38-39,left=40-41
    { "green_ghost", 42, 44, 2, "200" },  // right=42-43,left=44-45
    { "purple_ghost", 46, 48, 2, "200" },  // right=46-47,left=48-49
    { "chicken", 62, 63, 1, "CLUCK" },
    { "pikachu", 95, 96, 1, "PIKA" },
};
static const uint8_t HUNTER_SPRITE_COUNT = sizeof(HUNTER_SPRITES) / sizeof(HUNTER_SPRITES[0]);
// <<< END GENERATED: HUNTER_SPRITES >>>

// ---------------------------------------------------------------------------
// <<< GENERATED: PREY_SPRITES >>>
// SpriteInfo: name, rightStart, leftStart, frameCount, scoreText
static const SpriteInfo PREY_SPRITES[] = {
    { "blue_ghost", 50, 50, 2, "200" },  // frightened—flickers with gray_ghost
    { "butterfly_both", 78, 78, 2, "150" },
    { "monkey_both", 84, 84, 2, "OOH" },
};
static const uint8_t PREY_SPRITE_COUNT = sizeof(PREY_SPRITES) / sizeof(PREY_SPRITES[0]);

// Flicker sprites: alternate with prey near end of chase
static const uint8_t FLICKER_GRAY_GHOST_START  = 52;  // never standalone
static const uint8_t FLICKER_GRAY_GHOST_FRAMES = 2;

// Static sprites — sit at fixed X, eaten on overlap
static const SpriteInfo STATIC_SPRITES[] = {
    { "cherry", 86, 87, 1, "100" },
    { "apple", 88, 89, 1, "200" },
    { "beer_all", 90, 90, 4, "YUM" },
};
static const uint8_t STATIC_SPRITE_COUNT = sizeof(STATIC_SPRITES) / sizeof(STATIC_SPRITES[0]);
// <<< END GENERATED: PREY_SPRITES >>>

// Pac-Man sprite indices
static const uint8_t PAC_RIGHT_START  =  0;
static const uint8_t PAC_LEFT_START   =  4;
static const uint8_t PAC_UP_START     =  8;
static const uint8_t PAC_DOWN_START   = 12;
static const uint8_t PAC_FRAME_COUNT  =  4;
static const uint8_t PAC_DEATH_START  = 16;
static const uint8_t PAC_DEATH_FRAMES = 10;  // 16-25, frame 25=empty

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Lane system — 4-arm open/closed model
// ---------------------------------------------------------------------------
// The display has 4 arms radiating from the center (8,8):
//   ARM_LEFT:   x=0-7,  row 4-11  (travels right toward center)
//   ARM_RIGHT:  x=8-15, row 4-11  (travels right away from center)
//   ARM_TOP:    col 4-11, y=0-7   (travels down toward center)
//   ARM_BOTTOM: col 4-11, y=8-15  (travels down away from center)
//
// Any combination of 2, 3, or 4 arms produces:
//   2 adjacent arms  = L shape
//   2 opposite arms  = straight (H or V)
//   3 arms           = T shape
//   4 arms           = + shape
//
// Open arms: Pac-Man can travel them. Draw side barriers along the arm.
// Closed arms: blocked. Draw a single barrier pixel across the arm mouth at center.
// ---------------------------------------------------------------------------

struct Lane {
    bool left;    // arm open flags
    bool right;
    bool top;
    bool bottom;
    bool horizontal;  // primary entry axis for Pac-Man
};

static Lane g_lane;

// All legal shapes: any combo of 2+ arms
// Illegal: 0 arms, or single-arm dead ends (0001,0010,0100,1000)
static Lane randomLane() {
    // 10 legal multi-arm shapes
    static const Lane SHAPES[] = {
        {true, true, false,false,true},   // H straight
        {false,false,true, true, false},  // V straight
        {true, false,true, false,true},   // L top-left    (H entry)
        {false,true, true, false,false},  // L top-right   (V entry)
        {true, false,false,true, true},   // L bottom-left (H entry)
        {false,true, false,true, false},  // L bottom-right(V entry)
        {true, true, true, false,true},   // T (no bottom)
        {true, true, false,true, true},   // T (no top)
        {false,true, true, true, false},  // T (no left)
        {true, false,true, true, true},   // T (no right)
        {true, true, true, true, true},   // +
    };
    static const uint8_t N = sizeof(SHAPES)/sizeof(SHAPES[0]);
    Lane l = SHAPES[random(N)];
    // For L-shapes, randomly pick which open arm Pac-Man enters from
    // (horizontal flag may be flipped)
    // Count open arms
    uint8_t openH = (l.left?1:0)+(l.right?1:0);
    uint8_t openV = (l.top?1:0)+(l.bottom?1:0);
    if (openH > 0 && openV > 0) {
        // Both axes have open arms — pick random entry axis
        l.horizontal = (random(2) == 0);
    } else if (openH > 0) {
        l.horizontal = true;
    } else {
        l.horizontal = false;
    }
    return l;
}

// Geometry
static const int8_t LANE_ORIGIN   =  4;  // sprites occupy rows/cols 4-11
static const int8_t BARRIER_LO    =  2;  // barrier line before lane
static const int8_t BARRIER_HI    = 12;  // barrier line after lane
static const int8_t CENTER        =  8;  // center of display
static const CRGB   BARRIER_COLOR = CRGB(0, 0, 220);
static const CRGB   BLOCK_COLOR   = CRGB(0, 0, 180); // slightly dimmer block

static void drawBarriers(CRGB* leds) {
    // Exact pixel coordinates per arm (confirmed by user):
    //
    // LEFT open:   (0,2)-(1,2) and (0,12)-(1,12)       [2 short H segments]
    // LEFT closed: (2,3)-(2,11)                          [V line at x=2]
    //
    // RIGHT open:   (13,2)-(15,2) and (13,12)-(15,12)  [2 short H segments]
    // RIGHT closed: (12,3)-(12,11)                       [V line at x=12]
    //
    // TOP open:   (2,0)-(2,1) and (12,0)-(12,1)         [2 short V segments]
    // TOP closed: (3,2)-(11,2)                           [H line at y=2]
    //
    // BOTTOM open:   (2,13)-(2,15) and (12,13)-(12,15) [2 short V segments]
    // BOTTOM closed: (3,12)-(11,12)                      [H line at y=12]
    //
    // Corner pixels (2,2),(12,2),(2,12),(12,12):
    //   draw if exactly ONE of the two meeting arms is open.

    const CRGB bc = BARRIER_COLOR;

    // --- LEFT arm ---
    if (g_lane.left) {
        for (int x=0;x<=1;x++) { leds[XY(x,2)]=bc; leds[XY(x,12)]=bc; }
    } else {
        for (int y=3;y<=11;y++) leds[XY(2,y)]=BLOCK_COLOR;
    }

    // --- RIGHT arm ---
    if (g_lane.right) {
        for (int x=13;x<=15;x++) { leds[XY(x,2)]=bc; leds[XY(x,12)]=bc; }
    } else {
        for (int y=3;y<=11;y++) leds[XY(12,y)]=BLOCK_COLOR;
    }

    // --- TOP arm ---
    if (g_lane.top) {
        for (int y=0;y<=1;y++) { leds[XY(2,y)]=bc; leds[XY(12,y)]=bc; }
    } else {
        for (int x=3;x<=11;x++) leds[XY(x,2)]=BLOCK_COLOR;
    }

    // --- BOTTOM arm ---
    if (g_lane.bottom) {
        for (int y=13;y<=15;y++) { leds[XY(2,y)]=bc; leds[XY(12,y)]=bc; }
    } else {
        for (int x=3;x<=11;x++) leds[XY(x,12)]=BLOCK_COLOR;
    }

    // --- Corner pixels (2,2),(12,2),(2,12),(12,12) ---
    // Draw if exactly one of the two arms meeting at that corner is open
    // Top-left corner (2,2): left arm + top arm meet
    if (g_lane.left != g_lane.top)    leds[XY(2,2)]  = bc;
    // Top-right corner (12,2): right arm + top arm meet
    if (g_lane.right != g_lane.top)   leds[XY(12,2)] = bc;
    // Bottom-left corner (2,12): left arm + bottom arm meet
    if (g_lane.left != g_lane.bottom) leds[XY(2,12)] = bc;
    // Bottom-right corner (12,12): right arm + bottom arm meet
    if (g_lane.right != g_lane.bottom)leds[XY(12,12)]= bc;
}

// Blit actor using its own horizontal flag (not g_lane.horizontal)
// so turning actors use the correct axis after switchToArm
static void blitInLane(CRGB* leds, const uint32_t* frameData, int pos, bool horizontal) {
    if (horizontal)
        blitSprite8x8(leds, frameData, pos, LANE_ORIGIN);
    else
        blitSprite8x8(leds, frameData, LANE_ORIGIN, pos);
}

// Choose correct Pac-Man sprite for current heading and current arm axis
static uint8_t pacSpriteStart(bool movingPositive, bool horizontal) {
    if (horizontal)
        return movingPositive ? PAC_RIGHT_START : PAC_LEFT_START;
    else
        return movingPositive ? PAC_DOWN_START  : PAC_UP_START;
}

// Choose correct hunter/prey sprite for current heading
static uint8_t actorSpriteStart(const SpriteInfo& si, bool movingPositive) {
    return movingPositive ? si.rightStart : si.leftStart;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
struct PacmanSettings {
    uint8_t  numChasers    = 2;
    bool     autoSwitch    = true;
    uint8_t  chaseMode     = 0;
    uint8_t  hunterMask    = 0x07;
    uint8_t  preyMask      = 0x01;
    uint8_t  staticMask    = 0x07;
    char     interText[64] = "GO GO GO";
};
static PacmanSettings g_pac;

static void loadPacSettings() {
    Preferences p;
    p.begin("pacman", true);
    g_pac.numChasers = p.getUChar ("nchasers",   2);
    g_pac.autoSwitch = p.getBool  ("autoswitch", true);
    g_pac.chaseMode  = p.getUChar ("mode",       0);
    g_pac.hunterMask = p.getUChar ("hunters",    0x07);
    g_pac.preyMask   = p.getUChar ("prey",       0x01);
    g_pac.staticMask = p.getUChar ("statics",    0x07);
    String t         = p.getString("text",       "GO GO GO");
    strncpy(g_pac.interText, t.c_str(), 32); g_pac.interText[32]='\0';
    p.end();
}
static void savePacSettings() {
    Preferences p;
    p.begin("pacman", false);
    p.putUChar ("nchasers",   g_pac.numChasers);
    p.putBool  ("autoswitch", g_pac.autoSwitch);
    p.putUChar ("mode",       g_pac.chaseMode);
    p.putUChar ("hunters",    g_pac.hunterMask);
    p.putUChar ("prey",       g_pac.preyMask);
    p.putUChar ("statics",    g_pac.staticMask);
    p.putString("text",       g_pac.interText);
    p.end();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 3x5 font for score popups and small text (ASCII 32-90)
// ---------------------------------------------------------------------------
static const uint8_t FONT3x5[][3] = {
    {0x00,0x00,0x00},{0x00,0x17,0x00},{0x03,0x00,0x03},{0x1f,0x0a,0x1f},
    {0x0a,0x1f,0x05},{0x13,0x08,0x19},{0x0e,0x15,0x0a},{0x00,0x03,0x00},
    {0x00,0x0e,0x11},{0x11,0x0e,0x00},{0x0a,0x04,0x0a},{0x04,0x0e,0x04},
    {0x10,0x08,0x00},{0x04,0x04,0x04},{0x00,0x10,0x00},{0x18,0x04,0x03},
    {0x0e,0x11,0x0e},{0x12,0x1f,0x10},{0x19,0x15,0x12},{0x11,0x15,0x0e},
    {0x07,0x04,0x1f},{0x17,0x15,0x09},{0x0e,0x15,0x08},{0x01,0x1d,0x03},
    {0x0e,0x15,0x0e},{0x02,0x15,0x0e},{0x00,0x0a,0x00},{0x10,0x0a,0x00},
    {0x04,0x0a,0x11},{0x0a,0x0a,0x0a},{0x11,0x0a,0x04},{0x01,0x15,0x02},
    {0x0e,0x15,0x1e},{0x1e,0x05,0x1e},{0x1f,0x15,0x0e},{0x0e,0x11,0x11},
    {0x1f,0x11,0x0e},{0x1f,0x15,0x11},{0x1f,0x05,0x01},{0x0e,0x15,0x1c},
    {0x1f,0x04,0x1f},{0x11,0x1f,0x11},{0x08,0x10,0x0f},{0x1f,0x04,0x1b},
    {0x1f,0x10,0x10},{0x1f,0x02,0x1f},{0x1f,0x06,0x1f},{0x0e,0x11,0x0e},
    {0x1f,0x05,0x02},{0x0e,0x19,0x1e},{0x1f,0x05,0x1a},{0x12,0x15,0x09},
    {0x01,0x1f,0x01},{0x0f,0x10,0x1f},{0x07,0x18,0x07},{0x1f,0x0c,0x1f},
    {0x1b,0x04,0x1b},{0x03,0x1c,0x03},{0x19,0x15,0x13},
};
static const uint8_t SM_W = 3, SM_H = 5, SM_GAP = 1, SM_STEP = 4;

static uint8_t fontColSm(char c, uint8_t col) {
    if (c>='a'&&c<='z') c-=32;
    if (c<32||c>90) c=32;
    uint8_t idx=(uint8_t)(c-32);
    if (idx>=(uint8_t)(sizeof(FONT3x5)/SM_W)||col>=SM_W) return 0;
    return FONT3x5[idx][col];
}

// ---------------------------------------------------------------------------
// 5x7 font for scrolling message text (ASCII 32-90)
// ---------------------------------------------------------------------------
static const uint8_t FONT5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 space
    {0x00,0x00,0x5f,0x00,0x00}, // 33 !
    {0x00,0x07,0x00,0x07,0x00}, // 34 "
    {0x14,0x7f,0x14,0x7f,0x14}, // 35 #
    {0x24,0x2a,0x7f,0x2a,0x12}, // 36 $
    {0x23,0x13,0x08,0x64,0x62}, // 37 %
    {0x36,0x49,0x55,0x22,0x50}, // 38 &
    {0x00,0x05,0x03,0x00,0x00}, // 39 '
    {0x00,0x1c,0x22,0x41,0x00}, // 40 (
    {0x00,0x41,0x22,0x1c,0x00}, // 41 )
    {0x14,0x08,0x3e,0x08,0x14}, // 42 *
    {0x08,0x08,0x3e,0x08,0x08}, // 43 +
    {0x00,0x50,0x30,0x00,0x00}, // 44 ,
    {0x08,0x08,0x08,0x08,0x08}, // 45 -
    {0x00,0x60,0x60,0x00,0x00}, // 46 .
    {0x20,0x10,0x08,0x04,0x02}, // 47 /
    {0x3e,0x51,0x49,0x45,0x3e}, // 48 0
    {0x00,0x42,0x7f,0x40,0x00}, // 49 1
    {0x42,0x61,0x51,0x49,0x46}, // 50 2
    {0x21,0x41,0x45,0x4b,0x31}, // 51 3
    {0x18,0x14,0x12,0x7f,0x10}, // 52 4
    {0x27,0x45,0x45,0x45,0x39}, // 53 5
    {0x3c,0x4a,0x49,0x49,0x30}, // 54 6
    {0x01,0x71,0x09,0x05,0x03}, // 55 7
    {0x36,0x49,0x49,0x49,0x36}, // 56 8
    {0x06,0x49,0x49,0x29,0x1e}, // 57 9
    {0x00,0x36,0x36,0x00,0x00}, // 58 :
    {0x00,0x56,0x36,0x00,0x00}, // 59 ;
    {0x08,0x14,0x22,0x41,0x00}, // 60 <
    {0x14,0x14,0x14,0x14,0x14}, // 61 =
    {0x00,0x41,0x22,0x14,0x08}, // 62 >
    {0x02,0x01,0x51,0x09,0x06}, // 63 ?
    {0x32,0x49,0x79,0x41,0x3e}, // 64 @
    {0x7e,0x11,0x11,0x11,0x7e}, // 65 A
    {0x7f,0x49,0x49,0x49,0x36}, // 66 B
    {0x3e,0x41,0x41,0x41,0x22}, // 67 C
    {0x7f,0x41,0x41,0x22,0x1c}, // 68 D
    {0x7f,0x49,0x49,0x49,0x41}, // 69 E
    {0x7f,0x09,0x09,0x09,0x01}, // 70 F
    {0x3e,0x41,0x49,0x49,0x7a}, // 71 G
    {0x7f,0x08,0x08,0x08,0x7f}, // 72 H
    {0x00,0x41,0x7f,0x41,0x00}, // 73 I
    {0x20,0x40,0x41,0x3f,0x01}, // 74 J
    {0x7f,0x08,0x14,0x22,0x41}, // 75 K
    {0x7f,0x40,0x40,0x40,0x40}, // 76 L
    {0x7f,0x02,0x0c,0x02,0x7f}, // 77 M
    {0x7f,0x04,0x08,0x10,0x7f}, // 78 N
    {0x3e,0x41,0x41,0x41,0x3e}, // 79 O
    {0x7f,0x09,0x09,0x09,0x06}, // 80 P
    {0x3e,0x41,0x51,0x21,0x5e}, // 81 Q
    {0x7f,0x09,0x19,0x29,0x46}, // 82 R
    {0x46,0x49,0x49,0x49,0x31}, // 83 S
    {0x01,0x01,0x7f,0x01,0x01}, // 84 T
    {0x3f,0x40,0x40,0x40,0x3f}, // 85 U
    {0x1f,0x20,0x40,0x20,0x1f}, // 86 V
    {0x3f,0x40,0x38,0x40,0x3f}, // 87 W
    {0x63,0x14,0x08,0x14,0x63}, // 88 X
    {0x07,0x08,0x70,0x08,0x07}, // 89 Y
    {0x61,0x51,0x49,0x45,0x43}, // 90 Z
};
static const uint8_t FONT_W    = 5;
static const uint8_t FONT_H    = 7;
static const uint8_t FONT_GAP  = 1;
static const uint8_t CHAR_STEP = 6; // FONT_W + FONT_GAP

static uint8_t fontCol(char c, uint8_t col) {
    if (c>='a'&&c<='z') c-=32;
    if (c<32||c>90) c=32;
    uint8_t idx=(uint8_t)(c-32);
    if (idx>=(uint8_t)(sizeof(FONT5x7)/FONT_W)||col>=FONT_W) return 0;
    return FONT5x7[idx][col];
}


// ---------------------------------------------------------------------------
// Score popup — always floats upward
// ---------------------------------------------------------------------------
struct ScorePopup {
    bool     active=false;
    char     text[8]={0};
    uint8_t  len=0;
    int16_t  x=0;
    float    fy=0.0f;
    uint32_t startTime=0;
    bool     scrolling=false;
};
static ScorePopup g_popup;

static void triggerPopup(const char* txt, int16_t atX, int16_t atY) {
    if (!txt || !txt[0]) return;
    g_popup.active=true;
    strncpy(g_popup.text, txt, sizeof(g_popup.text)-1);
    g_popup.text[sizeof(g_popup.text)-1]='\0';
    for (char* p=g_popup.text;*p;p++) if(*p>='a'&&*p<='z') *p-=32;
    g_popup.len=(uint8_t)strlen(g_popup.text);
    g_popup.x=atX;
    g_popup.fy=(float)(atY);
    g_popup.startTime=millis();
    g_popup.scrolling=(g_popup.len>3);
}

static void drawPopup(CRGB* leds) {
    if (!g_popup.active) return;
    uint32_t elapsed=millis()-g_popup.startTime;
    uint32_t duration=g_popup.scrolling?800:500;
    if (elapsed>=duration) { g_popup.active=false; return; }
    uint8_t bright=255;
    if (elapsed>duration-200)
        bright=(uint8_t)map(elapsed,duration-200,duration,255,0);
    g_popup.fy-=0.1f;  // always float upward
    int16_t drawY=(int16_t)g_popup.fy;
    int16_t totalW=g_popup.len*SM_STEP-SM_GAP;
    int16_t startX=g_popup.x-totalW/2;
    for (uint8_t ci=0;ci<g_popup.len;ci++) {
        for (uint8_t col=0;col<SM_W;col++) {
            int16_t px=startX+ci*SM_STEP+col;
            if (px<0||px>=16) continue;
            uint8_t bits=fontColSm(g_popup.text[ci],col);
            for (uint8_t row=0;row<SM_H;row++) {
                int16_t py=drawY+row;
                if (py<0||py>=16) continue;
                if (bits&(1<<row))
                    leds[XY(px,py)]=CHSV(42,255,bright);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Actors
// ---------------------------------------------------------------------------
enum ActorRole { ROLE_PREY, ROLE_HUNTER, ROLE_STATIC };

// Arm identifiers
enum Arm { ARM_NONE=0, ARM_LEFT, ARM_RIGHT, ARM_TOP, ARM_BOTTOM };

// Is this arm open in the current lane?
static bool armOpen(Arm a) {
    switch(a) {
        case ARM_LEFT:   return g_lane.left;
        case ARM_RIGHT:  return g_lane.right;
        case ARM_TOP:    return g_lane.top;
        case ARM_BOTTOM: return g_lane.bottom;
        default:         return false;
    }
}

// Is this arm horizontal?
static bool armIsH(Arm a) { return a==ARM_LEFT||a==ARM_RIGHT; }

// Entry position for an arm (where actor starts, off-screen)
static float armEntryPos(Arm a) {
    switch(a) {
        case ARM_LEFT:   return -8.0f;
        case ARM_RIGHT:  return 16.0f;
        case ARM_TOP:    return -8.0f;
        case ARM_BOTTOM: return 16.0f;
        default:         return -8.0f;
    }
}

// Moving positive along this arm approaches center
static bool armPositiveToCenter(Arm a) {
    return (a==ARM_LEFT||a==ARM_TOP);
}

// Opposite arm
static Arm armOpposite(Arm a) {
    switch(a) {
        case ARM_LEFT:   return ARM_RIGHT;
        case ARM_RIGHT:  return ARM_LEFT;
        case ARM_TOP:    return ARM_BOTTOM;
        case ARM_BOTTOM: return ARM_TOP;
        default:         return ARM_NONE;
    }
}

struct Actor {
    float      pos;
    float      staticPos;
    uint8_t    sprStart;
    uint8_t    frameCount;
    uint8_t    frameIdx;
    bool       movingPositive;
    bool       active;
    ActorRole  role;
    bool       canFlicker;
    bool       flickerActive;
    const char* scoreText;
    uint8_t    rightStart;
    uint8_t    leftStart;
    // Phase 3: arm tracking
    Arm        currentArm;       // which arm this actor is on
    bool       horizontal;       // true=H axis, false=V axis
    bool       intersectDecided; // true once decision made at this intersection
    bool       intersectFleeable;// true if flee mode can reset the decision
    Arm        prevArm;          // arm before last intersection (avoid going back)
};

static const uint8_t MAX_ACTORS=6;
static Actor   g_pacman;
static Actor   g_actors[MAX_ACTORS];
static uint8_t g_numActors=0;
static uint32_t g_chaseStart=0;
static uint32_t g_chaseDuration=0;
static bool    g_pacFleeing=false;
static float   g_fleeOrigin=0.0f;
static bool    g_pacDead=false;
static uint32_t g_deathTimer=0;

// ---------------------------------------------------------------------------
// Dot and power pellet system
// ---------------------------------------------------------------------------
// Dots along open arms: bit N = dot at position N (0-15) along arm centerline
static uint16_t g_dots[4];  // [LEFT,RIGHT,TOP,BOTTOM] indexed by (Arm-1)
static bool     g_powerPellet=false;
static bool     g_powerActive=false;   // power pellet effect active
static uint32_t g_powerStart=0;
static const uint32_t POWER_DURATION  = 5000; // ms
static const uint32_t POWER_FLICKER   = 2000; // ms before end to flicker

// ---------------------------------------------------------------------------
// Dot system — clean single-scheme implementation
// ---------------------------------------------------------------------------
// Dots stored as bitmask per arm. Bit N = dot slot N (0-3 per arm).
// Left/Top arm: slot N covers screen positions N*2 .. N*2+1 (x or y = 0..5)
// Right/Bottom arm: slot N covers screen positions 10+N*2 .. (x or y = 10..15)
// Positions 6,7,8,9 are clear around the power pellet (wider gap requested)
// Center dot row/col = 7 for H arms, 7 for V arms (LANE_ORIGIN+3)

static const int DOT_CENTER_LANE = 7;  // center pixel of the lane (row or col)
static const int DOT_SLOTS = 3;        // 3 dots per arm side (at x/y = 0,2,4)

static void initDots() {
    for (int i=0;i<4;i++) g_dots[i]=0;
    // Left arm: dots at x = 0,2,4  (slots 0,1,2)
    if (g_lane.left)   g_dots[ARM_LEFT-1]   = 0x07; // bits 0,1,2
    // Right arm: dots at x = 11,13,15 (slots 0,1,2)
    if (g_lane.right)  g_dots[ARM_RIGHT-1]  = 0x07;
    // Top arm: dots at y = 0,2,4
    if (g_lane.top)    g_dots[ARM_TOP-1]    = 0x07;
    // Bottom arm: dots at y = 11,13,15
    if (g_lane.bottom) g_dots[ARM_BOTTOM-1] = 0x07;
    g_powerPellet = true;
}

static void drawDots(CRGB* leds) {
    CRGB dc = CRGB(70,70,70);
    // Left arm: x=0,2,4  row=DOT_CENTER_LANE
    if (g_lane.left)
        for(int s=0;s<DOT_SLOTS;s++)
            if(g_dots[ARM_LEFT-1]&(1<<s))
                leds[XY(s*2, DOT_CENTER_LANE)]=dc;
    // Right arm: x=11,13,15
    if (g_lane.right)
        for(int s=0;s<DOT_SLOTS;s++)
            if(g_dots[ARM_RIGHT-1]&(1<<s))
                leds[XY(11+s*2, DOT_CENTER_LANE)]=dc;
    // Top arm: y=0,2,4  col=DOT_CENTER_LANE
    if (g_lane.top)
        for(int s=0;s<DOT_SLOTS;s++)
            if(g_dots[ARM_TOP-1]&(1<<s))
                leds[XY(DOT_CENTER_LANE, s*2)]=dc;
    // Bottom arm: y=11,13,15
    if (g_lane.bottom)
        for(int s=0;s<DOT_SLOTS;s++)
            if(g_dots[ARM_BOTTOM-1]&(1<<s))
                leds[XY(DOT_CENTER_LANE, 11+s*2)]=dc;
    // Power pellet: 2x2 at center (7,7)-(8,8)
    if (g_powerPellet) {
        // 3x3 antialiased circle centered at (7,7)
        // corners=30%, cardinals=65%, center=100%
        leds[XY(7,7)] = CRGB(255,255,255);          // center
        leds[XY(6,7)] = CRGB(165,165,165);          // left
        leds[XY(8,7)] = CRGB(165,165,165);          // right
        leds[XY(7,6)] = CRGB(165,165,165);          // top
        leds[XY(7,8)] = CRGB(165,165,165);          // bottom
        leds[XY(6,6)] = CRGB(76,76,76);             // top-left corner
        leds[XY(8,6)] = CRGB(76,76,76);             // top-right corner
        leds[XY(6,8)] = CRGB(76,76,76);             // bottom-left corner
        leds[XY(8,8)] = CRGB(76,76,76);             // bottom-right corner
    }
}

static void eatDotAt(Arm arm, float pos) {
    if (arm==ARM_NONE) return;
    int p = (int)pos;
    int idx = arm-1;
    if (arm==ARM_LEFT||arm==ARM_TOP) {
        // Slots at screen positions 0,2,4 → slot = p/2
        int slot = p/2;
        if(slot>=0 && slot<DOT_SLOTS) g_dots[idx] &= ~(1<<slot);
    } else {
        // Slots at screen positions 11,13,15 → slot = (p-11)/2
        int slot = (p-11)/2;
        if(slot>=0 && slot<DOT_SLOTS) g_dots[idx] &= ~(1<<slot);
    }
}

// ---------------------------------------------------------------------------
// Intersection navigation
// ---------------------------------------------------------------------------
static const float CENTER_POS = 7.5f; // center of display along arm axis

// Get list of open arms that are perpendicular to current arm (for turning)
// plus continuation of current arm if open
static uint8_t getExitArms(Actor& actor, Arm* exits) {
    uint8_t n=0;
    Arm all[4]={ARM_LEFT,ARM_RIGHT,ARM_TOP,ARM_BOTTOM};
    for(uint8_t i=0;i<4;i++) {
        if(all[i]==actor.currentArm) continue; // not the arm we're on
        if(all[i]==actor.prevArm)    continue; // not where we just came from
        if(armOpen(all[i])) exits[n++]=all[i];
    }
    // If nothing valid (straight lane, both exits excluded), allow opposite
    if(n==0 && armOpen(armOpposite(actor.currentArm)))
        exits[n++]=armOpposite(actor.currentArm);
    return n;
}

// Choose exit arm for Pac-Man based on game state
static Arm choosePacExit(Actor& pac) {
    Arm exits[4]; uint8_t n=getExitArms(pac,exits);
    if(n==0) return armOpposite(pac.currentArm);
    if(n==1) return exits[0];
    // Flee hunters
    float nearestHunter=999.f; Arm hunterArm=ARM_NONE;
    for(uint8_t i=0;i<g_numActors;i++) {
        Actor& a=g_actors[i]; if(!a.active||a.role!=ROLE_HUNTER) continue;
        float d=fabsf(pac.pos-a.pos);
        if(d<nearestHunter){nearestHunter=d;hunterArm=a.currentArm;}
    }
    if(nearestHunter<15.f&&!g_powerActive) {
        for(uint8_t i=0;i<n;i++) if(exits[i]!=hunterArm) return exits[i];
        return exits[0];
    }
    // Chase prey
    if(g_pac.chaseMode==0) {
        float nearestPrey=999.f; Arm preyArm=ARM_NONE;
        for(uint8_t i=0;i<g_numActors;i++) {
            Actor& a=g_actors[i]; if(!a.active||a.role==ROLE_HUNTER) continue;
            float d=fabsf(pac.pos-a.pos);
            if(d<nearestPrey){nearestPrey=d;preyArm=a.currentArm;}
        }
        if(preyArm!=ARM_NONE)
            for(uint8_t i=0;i<n;i++) if(exits[i]==preyArm) return exits[i];
    }
    return exits[random(n)];
}

static Arm chooseHunterExit(Actor& actor) {
    Arm exits[4]; uint8_t n=getExitArms(actor,exits);
    if(n==0) return armOpposite(actor.currentArm);
    if(n==1) return exits[0];
    for(uint8_t i=0;i<n;i++) if(exits[i]==g_pacman.currentArm) return exits[i];
    return exits[random(n)];
}

static Arm choosePreyExit(Actor& actor) {
    Arm exits[4]; uint8_t n=getExitArms(actor,exits);
    if(n==0) return armOpposite(actor.currentArm);
    if(n==1) return exits[0];
    for(uint8_t i=0;i<n;i++) if(exits[i]!=g_pacman.currentArm) return exits[i];
    return exits[random(n)];
}

// Transition an actor to a new arm after intersection decision
static void switchToArm(Actor& a, Arm newArm) {
    a.prevArm           = a.currentArm;
    a.currentArm        = newArm;
    a.horizontal        = armIsH(newArm);
    a.movingPositive    = !armPositiveToCenter(newArm);
    // Keep pos as-is (~7.5) — don't teleport, just change axis and direction.
    // intersectDecided stays true until pos moves >3px away from center.
    a.intersectDecided  = true;
    a.intersectFleeable = false;
}

// ---------------------------------------------------------------------------
// Inter-chase scroll
// ---------------------------------------------------------------------------
enum ChasePhase { PHASE_SCROLL, PHASE_REVEAL, PHASE_CHASE, PHASE_DEATH };
static ChasePhase g_phase=PHASE_SCROLL;
static uint32_t   g_revealStart=0;
static bool       g_initialized=false;
static uint8_t    g_chaseCount=0;

// Scroll state
static float      g_scrollX=16.0f;      // current X offset (float for smooth decel)
static float      g_scrollSpeed=1.0f;   // pixels per frame (decelerates to 0)
static bool       g_scrollFromLeft=true; // direction: true=enter from right, scroll left
static bool       g_scrollStopped=false; // true when text has stopped centered
static uint32_t   g_scrollStopTime=0;   // when it stopped
static uint8_t    g_scrollHue=0;
static uint8_t    g_scrollColorMode=0; // 0=rainbow,1=warm,2=cool,3=red,4=twotone
static CRGB       g_scrollColor1, g_scrollColor2; // for solid/twotone modes

static const char* scrollMsg() {
    return (g_chaseCount%6==0) ? "READY!" : g_pac.interText;
}

// Total pixel width of message
static int16_t msgWidth(const char* msg) {
    int16_t len=(int16_t)strlen(msg);
    // READY! uses small font; custom text uses large font
    bool sm = (g_chaseCount % 6 == 0);
    return sm ? (len * SM_STEP - SM_GAP) : (len * CHAR_STEP - FONT_GAP);
}

// X position that centers the message on the 16px display

static void initScroll() {
    const char* msg = scrollMsg();
    g_scrollFromLeft = (random(2)==0);
    g_scrollStopped  = false;
    g_scrollSpeed    = 1.5f;
    // Pick random scroll color mode
    uint8_t r = random(10);
    if      (r < 4) { g_scrollColorMode=0; }  // 40% rainbow
    else if (r < 6) {                          // 20% warm solid
        g_scrollColorMode=1;
        uint8_t hue = random(3)==0?64:(random(2)==0?32:42); // yellow/orange/gold
        g_scrollColor1=CHSV(hue,255,220);
    } else if (r < 8) {                        // 20% cool solid
        g_scrollColorMode=2;
        uint8_t hue = 128+random(64); // cyan..blue..purple
        g_scrollColor1=CHSV(hue,255,220);
    } else if (r < 9) {                        // 10% red/pink
        g_scrollColorMode=3;
        uint8_t hue = random(2)==0?0:224; // red or pink
        g_scrollColor1=CHSV(hue,255,220);
    } else {                                   // 10% two-tone
        g_scrollColorMode=4;
        g_scrollColor1=CHSV(random(256),255,220);
        g_scrollColor2=CHSV(random(256),255,220);
    }

    if (g_scrollFromLeft) {
        // Enters from right, scrolls left, stops when left edge of text = col 0
        g_scrollX = 16.0f;
    } else {
        // Enters from left, scrolls right, stops when right edge of text = col 15
        g_scrollX = -(float)msgWidth(msg);
    }
}

static void drawScrollText(CRGB* leds) {
    fill_solid(leds,NUM_LEDS,CRGB::Black);
    g_scrollHue++;
    const char* msg=scrollMsg();
    int16_t len=(int16_t)strlen(msg);
    int16_t w = msgWidth(msg);

    // Target: left edge when from-right (scrollX=0), right edge when from-left
    // Stop when the TRAILING edge of the text reaches the screen edge.
    // Scrolling left (from right): trailing = right side, stop at col 15
    //   scrollX = 16 - msgWidth  (right edge = scrollX + w - 1 = 15)
    // Scrolling right (from left): trailing = left side, stop at col 0
    //   scrollX = 0
    float targetX = g_scrollFromLeft ? (float)(16 - w) : 0.0f;

    if (!g_scrollStopped) {
        if (g_scrollFromLeft) {
            g_scrollX -= g_scrollSpeed;
            float gap = g_scrollX - targetX;
            if (gap < 8.0f) g_scrollSpeed = max(0.25f, g_scrollSpeed * 0.85f);
            if (g_scrollX <= targetX) {
                g_scrollX = targetX;
                g_scrollStopped = true;
                g_scrollStopTime = millis();
            }
        } else {
            g_scrollX += g_scrollSpeed;
            float gap = targetX - g_scrollX;
            if (gap < 8.0f) g_scrollSpeed = max(0.25f, g_scrollSpeed * 0.85f);
            if (g_scrollX >= targetX) {
                g_scrollX = targetX;
                g_scrollStopped = true;
                g_scrollStopTime = millis();
            }
        }
    }

    // READY! uses 3x5 small font; custom message uses 5x7 large font
    bool useSmFont = (msg == scrollMsg() && g_chaseCount % 6 == 0);
    if (useSmFont) {
        // 3x5 font, centered vertically
        int16_t startRow = (16 - SM_H) / 2;
        for (int16_t col=0; col<16; col++) {
            int16_t srcCol = col - (int16_t)g_scrollX;
            int16_t smW = (int16_t)(len * SM_STEP - SM_GAP);
            if (srcCol < 0 || srcCol >= smW + SM_STEP) continue;
            if (srcCol >= len * SM_STEP) continue;
            uint8_t ci = srcCol / SM_STEP;
            uint8_t cc = srcCol % SM_STEP;
            if (cc >= SM_W) continue;
            uint8_t bits = fontColSm(msg[ci], cc);
            for (uint8_t row=0; row<SM_H; row++) {
                if (bits & (1<<row)) {
                    // READY! always uses rainbow
                    leds[XY(col, startRow+row)] = CHSV(g_scrollHue+col*8,255,220);
                }
            }
        }
    } else {
        // 5x7 font, centered vertically (rows 4-10)
        int16_t startRow = (16 - FONT_H) / 2;
        for (int16_t col=0; col<16; col++) {
            int16_t srcCol = col - (int16_t)g_scrollX;
            if (srcCol < 0 || srcCol >= w + CHAR_STEP) continue;
            if (srcCol >= len * CHAR_STEP) continue;
            uint8_t ci  = srcCol / CHAR_STEP;
            uint8_t cc  = srcCol % CHAR_STEP;
            if (cc >= FONT_W) continue;
            uint8_t bits = fontCol(msg[ci], cc);
            for (uint8_t row=0; row<FONT_H; row++) {
                if (bits & (1<<row)) {
                    CRGB c;
                    switch(g_scrollColorMode) {
                        case 0:  c=CHSV(g_scrollHue+col*8,255,220); break;
                        case 1: case 2: case 3: c=g_scrollColor1; break;
                        case 4:  c=(ci%2==0)?g_scrollColor1:g_scrollColor2; break;
                        default: c=CHSV(g_scrollHue+col*8,255,220); break;
                    }
                    leds[XY(col, startRow+row)] = c;
                }
            }
        }
    }
    FastLED.show();
}

// ---------------------------------------------------------------------------
// Chase init
// ---------------------------------------------------------------------------
static void initChase() {
    bool pacChases=(g_pac.chaseMode==0);
    g_pacFleeing=false;
    g_fleeOrigin=0.0f;
    g_pacDead=false;
    g_powerActive=false;
    g_lane=randomLane();
    initDots();

    // Helper to init a new actor on an entry arm
    auto initActor = [](Actor& a, Arm entryArm) {
        a.currentArm     = entryArm;
        a.horizontal     = armIsH(entryArm);
        // Moving toward center from entry arm
        a.movingPositive = armPositiveToCenter(entryArm);
        a.pos            = armEntryPos(entryArm);
        a.intersectDecided  = false;
        a.intersectFleeable = false;
        a.prevArm           = ARM_NONE;
    };

    // --- Pac-Man ---
    // Pick a random open arm on the primary axis for entry
    Arm pacEntry = ARM_NONE;
    {
        Arm opts[2]; uint8_t n=0;
        if(g_lane.horizontal) {
            if(g_lane.left)  opts[n++]=ARM_LEFT;
            if(g_lane.right) opts[n++]=ARM_RIGHT;
        } else {
            if(g_lane.top)    opts[n++]=ARM_TOP;
            if(g_lane.bottom) opts[n++]=ARM_BOTTOM;
        }
        if(n>0) pacEntry=opts[random(n)];
        else {
            // Fallback: any open arm
            Arm all[4]={ARM_LEFT,ARM_RIGHT,ARM_TOP,ARM_BOTTOM};
            for(uint8_t i=0;i<4;i++) if(armOpen(all[i])){pacEntry=all[i];break;}
        }
    }
    g_pacman.rightStart   = PAC_RIGHT_START;
    g_pacman.leftStart    = PAC_LEFT_START;
    g_pacman.frameCount   = PAC_FRAME_COUNT;
    g_pacman.frameIdx     = 0;
    g_pacman.active       = true;
    g_pacman.canFlicker   = false;
    g_pacman.flickerActive= false;
    g_pacman.role         = ROLE_PREY;
    g_pacman.scoreText    = nullptr;
    initActor(g_pacman, pacEntry);
    g_pacman.sprStart     = pacSpriteStart(g_pacman.movingPositive, g_pacman.horizontal);

    g_numActors=0;
    uint8_t nc=min((uint8_t)4,g_pac.numChasers);

    // Build list of available entry arms (opposite side from pac)
    Arm pacOpp = armOpposite(pacEntry);

    if (pacChases) {
        uint8_t placed=0;
        for (uint8_t pi=0;pi<PREY_SPRITE_COUNT&&placed<nc;pi++) {
            if (!(g_pac.preyMask&(1<<pi))) continue;
            Actor& a=g_actors[g_numActors++];
            a.rightStart   =PREY_SPRITES[pi].rightStart;
            a.leftStart    =PREY_SPRITES[pi].leftStart;
            a.frameCount   =PREY_SPRITES[pi].frameCount;
            a.frameIdx     =0;
            a.active       =true;
            a.role         =ROLE_PREY;
            a.canFlicker   =(pi==0);
            a.flickerActive=false;
            a.scoreText    =PREY_SPRITES[pi].scoreText;
            // Prey start from the opposite arm, fleeing away
            Arm preyArm = armOpen(pacOpp) ? pacOpp : pacEntry;
            initActor(a, preyArm);
            // Flee away from center (reverse direction)
            a.movingPositive = !armPositiveToCenter(preyArm);
            a.pos = (a.movingPositive) ? 16.0f + placed*10.0f : -8.0f - placed*10.0f;
            a.sprStart = a.movingPositive ? a.rightStart : a.leftStart;
            placed++;
        }
    } else {
        uint8_t placed=0;
        for (uint8_t hi=0;hi<HUNTER_SPRITE_COUNT&&placed<nc;hi++) {
            if (!(g_pac.hunterMask&(1<<hi))) continue;
            Actor& a=g_actors[g_numActors++];
            a.rightStart   =HUNTER_SPRITES[hi].rightStart;
            a.leftStart    =HUNTER_SPRITES[hi].leftStart;
            a.frameCount   =HUNTER_SPRITES[hi].frameCount;
            a.frameIdx     =0;
            a.active       =true;
            a.role         =ROLE_HUNTER;
            a.canFlicker   =false;
            a.flickerActive=false;
            a.scoreText    =HUNTER_SPRITES[hi].scoreText;
            // Hunters enter from same arm as pac (behind) or opposite
            Arm huntArm = armOpen(pacOpp) ? pacOpp : pacEntry;
            initActor(a, huntArm);
            a.pos = armEntryPos(huntArm) + (placed * (armPositiveToCenter(huntArm)?-10.0f:10.0f));
            a.sprStart = a.movingPositive ? a.rightStart : a.leftStart;
            placed++;
        }
    }

    // One static item on a random open arm
    uint8_t enabled[STATIC_SPRITE_COUNT], eCount=0;
    for (uint8_t si=0;si<STATIC_SPRITE_COUNT;si++)
        if (g_pac.staticMask&(1<<si)) enabled[eCount++]=si;
    if (eCount>0&&g_numActors<MAX_ACTORS) {
        uint8_t si=enabled[random(eCount)];
        // Pick a random open arm for the static item
        Arm sArms[4]; uint8_t sn=0;
        Arm all[4]={ARM_LEFT,ARM_RIGHT,ARM_TOP,ARM_BOTTOM};
        for(uint8_t i=0;i<4;i++) if(armOpen(all[i])) sArms[sn++]=all[i];
        if(sn>0) {
            Arm sArm=sArms[random(sn)];
            float sp = armPositiveToCenter(sArm) ? 2.0f+random(4) : 10.0f+random(4);
            Actor& a=g_actors[g_numActors++];
            a.rightStart   =STATIC_SPRITES[si].rightStart;
            a.leftStart    =STATIC_SPRITES[si].leftStart;
            a.frameCount   =STATIC_SPRITES[si].frameCount;
            a.frameIdx     =0;
            a.active       =true;
            a.role         =ROLE_STATIC;
            a.canFlicker   =false;
            a.flickerActive=false;
            a.scoreText    =STATIC_SPRITES[si].scoreText;
            a.currentArm   =sArm;
            a.horizontal   =armIsH(sArm);
            a.pos          =sp;
            a.staticPos    =sp;
            a.movingPositive=true;
            a.sprStart     =a.rightStart;
            a.intersectDecided=false;
            a.intersectFleeable=false;
            a.prevArm=ARM_NONE;
        }
    }

    g_chaseStart=millis();
    g_chaseDuration=(uint32_t)((32.0f/0.18f)*33); // rough estimate
}

// ---------------------------------------------------------------------------
// Reveal phase — show barriers + dots, hold 1s before actors enter
// ---------------------------------------------------------------------------
static void drawReveal(CRGB* leds) {
    fill_solid(leds,NUM_LEDS,CRGB::Black);
    drawBarriers(leds);
    drawDots(leds);
    FastLED.show();
    if (millis()-g_revealStart > 1000) {
        g_phase=PHASE_CHASE;
        g_chaseStart=millis();
    }
}

// ---------------------------------------------------------------------------
// Death animation
// ---------------------------------------------------------------------------
static void drawDeath(CRGB* leds) {
    fill_solid(leds,NUM_LEDS,CRGB::Black);
    drawBarriers(leds);
    drawDots(leds);
    uint32_t elapsed=millis()-g_deathTimer;
    uint8_t frame=(uint8_t)(elapsed/110);
    if (frame>=PAC_DEATH_FRAMES) {
        if (elapsed>(uint32_t)(PAC_DEATH_FRAMES*110+500)) {
            g_phase=PHASE_SCROLL; initScroll();
            g_chaseCount++;
            if (g_pac.autoSwitch) g_pac.chaseMode=1-g_pac.chaseMode;
            return;
        }
        frame=PAC_DEATH_FRAMES-1;
    }
    // Draw at pac's screen position
    int px = (int)g_pacman.pos;
    int py = LANE_ORIGIN;
    if (!g_pacman.horizontal) { py=px; px=LANE_ORIGIN; }
    blitSprite8x8(leds, packman6_sprites8x8_data[PAC_DEATH_START+frame], px, py);
    FastLED.show();
}

// ---------------------------------------------------------------------------
// Blit actor at its current arm position
// ---------------------------------------------------------------------------
static void blitActor(CRGB* leds, Actor& a, uint8_t sprIdx) {
    int p = (int)a.pos;
    if (a.horizontal) {
        blitSprite8x8(leds, packman6_sprites8x8_data[sprIdx], p, LANE_ORIGIN);
    } else {
        blitSprite8x8(leds, packman6_sprites8x8_data[sprIdx], LANE_ORIGIN, p);
    }
}

// ---------------------------------------------------------------------------
// Chase frame
// ---------------------------------------------------------------------------
static void drawChase(CRGB* leds) {
    fill_solid(leds,NUM_LEDS,CRGB::Black);
    drawBarriers(leds);
    drawDots(leds);

    float pacSpeed  = g_powerActive ? 0.30f : 0.26f;
    float preySpeed = g_powerActive ? 0.16f : 0.20f;
    float huntSpeed = g_powerActive ? 0.16f : 0.30f; // hunters slow when frightened

    // Power pellet timer
    uint32_t powerElapsed = g_powerActive ? millis()-g_powerStart : 0;
    bool powerExpired = g_powerActive && (powerElapsed >= POWER_DURATION);
    if (powerExpired) {
        g_powerActive=false;
        // Reset all frightened hunters
        for(uint8_t i=0;i<g_numActors;i++) {
            if(g_actors[i].role==ROLE_HUNTER) {
                g_actors[i].canFlicker=false;
                g_actors[i].flickerActive=false;
            }
        }
    }

    // Flicker for frightened ghosts (power pellet warning)
    bool flickerWarning = g_powerActive && (powerElapsed > POWER_DURATION-POWER_FLICKER);
    static uint8_t flickerTick=0; flickerTick++;
    bool flickerPhase=(flickerTick&8)!=0;

    // Advance frame timer
    static uint8_t frameTimer=0; frameTimer++;
    bool advance=(frameTimer>=4);
    if(advance) { frameTimer=0; g_pacman.frameIdx=(g_pacman.frameIdx+1)%PAC_FRAME_COUNT; }

    // ---------------------------------------------------------------------------
    // Intersection detection and navigation
    // ---------------------------------------------------------------------------
    const float INTER_THRESHOLD = 1.0f; // how close to center triggers intersection
    const float CENTER_COORD    = 7.5f;

    // Check if actor is at intersection
    auto nearCenter=[&](float pos)->bool {
        return fabsf(pos-CENTER_COORD)<INTER_THRESHOLD;
    };

    // --- Pac-Man intersection ---
    // Reset decided only after pac has moved >3px from center on the new arm
    if(g_pacman.intersectDecided && fabsf(g_pacman.pos-7.5f)>3.5f)
        g_pacman.intersectDecided=false;

    if(nearCenter(g_pacman.pos) && !g_pacman.intersectDecided) {
        // Power pellet
        if(g_powerPellet) {
            g_powerPellet=false;
            g_powerActive=true;
            g_powerStart=millis();
            triggerPopup("POW!", 8, 8);
            for(uint8_t i=0;i<g_numActors;i++)
                if(g_actors[i].role==ROLE_HUNTER)
                    g_actors[i].canFlicker=true;
        }
        eatDotAt(g_pacman.currentArm, g_pacman.pos);
        // Make turn decision immediately — no pause
        Arm exit=choosePacExit(g_pacman);
        switchToArm(g_pacman, exit);
        g_pacman.sprStart=pacSpriteStart(g_pacman.movingPositive, g_pacman.horizontal);
    } else {
        // Move Pac-Man
        g_pacman.pos += g_pacman.movingPositive ? pacSpeed : -pacSpeed;
        eatDotAt(g_pacman.currentArm, g_pacman.pos);
        g_pacman.sprStart=pacSpriteStart(g_pacman.movingPositive, g_pacman.horizontal);
    }

    // --- Actor updates ---
    for(uint8_t i=0;i<g_numActors;i++) {
        Actor& a=g_actors[i];
        if(!a.active) continue;

        if(a.role==ROLE_STATIC) {
            // Check if Pac-Man overlaps
            bool sameArm = (a.currentArm==g_pacman.currentArm);
            bool sameH   = (a.horizontal==g_pacman.horizontal);
            float dist   = fabsf(g_pacman.pos - a.staticPos);
            if((sameArm||sameH) && dist<5.0f) {
                int16_t px=a.horizontal?(int16_t)a.staticPos+4:LANE_ORIGIN+4;
                int16_t py=a.horizontal?LANE_ORIGIN:(int16_t)a.staticPos+4;
                triggerPopup(a.scoreText,px,py);
                a.active=false;
                continue;
            }
            if(advance) a.frameIdx=(a.frameIdx+1)%a.frameCount;
            a.sprStart=a.rightStart+(a.frameIdx%a.frameCount);
            continue;
        }

        // Intersection handling for moving actors
        // Reset decided when actor moves away from center
        if(a.intersectDecided && fabsf(a.pos-7.5f)>3.5f)
            a.intersectDecided=false;

        if(nearCenter(a.pos) && !a.intersectDecided) {
            Arm exit;
            if(a.role==ROLE_HUNTER) {
                exit = g_powerActive ? choosePreyExit(a)
                                    : chooseHunterExit(a);
            } else {
                exit = choosePreyExit(a);
            }
            switchToArm(a, exit);
        } else {
            // Move
            float spd = (a.role==ROLE_HUNTER) ? huntSpeed : preySpeed;
            a.pos += a.movingPositive ? spd : -spd;
        }

        // Update sprite
        if(advance) a.frameIdx=(a.frameIdx+1)%a.frameCount;
        if(a.role==ROLE_HUNTER) {
            bool frightened = g_powerActive;
            bool flicker    = frightened && flickerWarning && flickerPhase;
            if(flicker) {
                a.sprStart=FLICKER_GRAY_GHOST_START+(a.frameIdx%FLICKER_GRAY_GHOST_FRAMES);
            } else if(frightened) {
                a.sprStart=PREY_SPRITES[0].rightStart+(a.frameIdx%PREY_SPRITES[0].frameCount);
            } else {
                uint8_t base=a.movingPositive?a.rightStart:a.leftStart;
                a.sprStart=base+(a.frameIdx%a.frameCount);
            }
            // Eat by Pac-Man during power mode
            if(g_powerActive && fabsf(g_pacman.pos-a.pos)<5.0f
               && a.currentArm==g_pacman.currentArm) {
                triggerPopup(a.scoreText, 8, 8);
                a.active=false;
                continue;
            }
            // Kill Pac-Man if hunter catches him (not frightened)
            if(!g_powerActive && fabsf(g_pacman.pos-a.pos)<5.0f
               && a.currentArm==g_pacman.currentArm) {
                g_pacDead=true;
                g_deathTimer=millis();
                g_phase=PHASE_DEATH;
                return;
            }
        } else { // ROLE_PREY
            if(a.canFlicker) {
                // Blue ghost flicker near end of chase duration
                uint32_t elapsed=millis()-g_chaseStart;
                bool flic=(elapsed>g_chaseDuration*70/100)&&flickerPhase;
                a.flickerActive=flic;
            }
            if(a.canFlicker&&a.flickerActive) {
                a.sprStart=FLICKER_GRAY_GHOST_START+(a.frameIdx%FLICKER_GRAY_GHOST_FRAMES);
            } else {
                uint8_t base=a.movingPositive?a.rightStart:a.leftStart;
                a.sprStart=base+(a.frameIdx%a.frameCount);
            }
            // Pac-Man eats prey
            if(g_pac.chaseMode==0 && fabsf(g_pacman.pos-a.pos)<5.0f
               && a.currentArm==g_pacman.currentArm) {
                int16_t px=a.horizontal?(int16_t)a.pos+4:LANE_ORIGIN+4;
                int16_t py=a.horizontal?LANE_ORIGIN:(int16_t)a.pos+4;
                triggerPopup(a.scoreText,px,py);
                a.active=false;
                continue;
            }
        }
    }

    // --- Draw actors ---
    for(uint8_t i=0;i<g_numActors;i++) {
        Actor& a=g_actors[i];
        if(!a.active) continue;
        blitActor(leds, a, a.sprStart);
    }

    // Draw Pac-Man
    uint8_t pSpr=g_pacman.sprStart+(g_pacman.frameIdx%PAC_FRAME_COUNT);
    blitActor(leds, g_pacman, pSpr);

    drawPopup(leds);
    FastLED.show();

    // End chase: wait until ALL actors (including Pac-Man) have exited
    bool allGone = !g_pacman.active || (g_pacman.pos>16.0f||g_pacman.pos<-8.0f);
    if(allGone) {
        // Check all moving actors also gone
        for(uint8_t i=0;i<g_numActors;i++) {
            Actor& a=g_actors[i];
            if(!a.active||a.role==ROLE_STATIC) continue;
            if(a.pos>=-8.0f&&a.pos<=16.0f) { allGone=false; break; }
        }
    }
    if(allGone) {
        // Brief hold then scroll
        delay(300);
        g_phase=PHASE_SCROLL; initScroll();
        g_chaseCount++;
        if(g_pac.autoSwitch) g_pac.chaseMode=1-g_pac.chaseMode;
    }
}


// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
void pacman(CRGB* leds) {
    if (!g_initialized) {
        loadPacSettings();
        g_phase=PHASE_SCROLL;
        g_chaseCount=0;
        initScroll();
        g_initialized=true;
    }

    switch (g_phase) {
        case PHASE_SCROLL: {
            drawScrollText(leds);
            if (g_scrollStopped &&
                millis() - g_scrollStopTime > 1500) {
                // Transition to reveal phase — init chase first so dots/barriers are ready
                initChase();
                g_revealStart = millis();
                g_phase = PHASE_REVEAL;
            }
            delay(40);
            break;
        }
        case PHASE_REVEAL:
            drawReveal(leds);
            delay(40);
            break;
        case PHASE_CHASE:
            drawChase(leds);
            delay(22);
            break;
        case PHASE_DEATH:
            drawDeath(leds);
            delay(22);
            break;
    }
}

// ---------------------------------------------------------------------------
// Web API + UI
// ---------------------------------------------------------------------------
void pacmanSetup(AsyncWebServer* server) {
    loadPacSettings();

    server->on("/pacman/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        String j="{";
        j+="\"mode\":"      +String(g_pac.chaseMode)            +",";
        j+="\"nchasers\":"  +String(g_pac.numChasers)           +",";
        j+="\"autoswitch\":"+String(g_pac.autoSwitch?"true":"false")+",";
        j+="\"hunterMask\":"+String(g_pac.hunterMask)           +",";
        j+="\"preyMask\":"  +String(g_pac.preyMask)             +",";
        j+="\"staticMask\":"+String(g_pac.staticMask)           +",";
        j+="\"text\":\""   +String(g_pac.interText)             +"\",";
        j+="\"hunters\":[";
        for (uint8_t i=0;i<HUNTER_SPRITE_COUNT;i++) {
            if (i) j+=",";
            j+="{\"idx\":"+String(i)+",\"name\":\""+HUNTER_SPRITES[i].name+
               "\",\"score\":\""+HUNTER_SPRITES[i].scoreText+
               "\",\"on\":"+(g_pac.hunterMask&(1<<i)?"true":"false")+"}";
        }
        j+="],\"prey\":[";
        for (uint8_t i=0;i<PREY_SPRITE_COUNT;i++) {
            if (i) j+=",";
            j+="{\"idx\":"+String(i)+",\"name\":\""+PREY_SPRITES[i].name+
               "\",\"score\":\""+PREY_SPRITES[i].scoreText+
               "\",\"on\":"+(g_pac.preyMask&(1<<i)?"true":"false")+"}";
        }
        j+="],\"statics\":[";
        for (uint8_t i=0;i<STATIC_SPRITE_COUNT;i++) {
            if (i) j+=",";
            j+="{\"idx\":"+String(i)+",\"name\":\""+STATIC_SPRITES[i].name+
               "\",\"score\":\""+STATIC_SPRITES[i].scoreText+
               "\",\"on\":"+(g_pac.staticMask&(1<<i)?"true":"false")+"}";
        }
        j+="]}";
        req->send(200,"application/json",j);
    });

    server->on("/pacman/set", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (req->hasParam("mode"))
            g_pac.chaseMode =(uint8_t)req->getParam("mode")->value().toInt();
        if (req->hasParam("nchasers"))
            g_pac.numChasers=constrain((uint8_t)req->getParam("nchasers")->value().toInt(),1,4);
        if (req->hasParam("autoswitch"))
            g_pac.autoSwitch=req->getParam("autoswitch")->value()=="1";
        if (req->hasParam("hunters"))
            g_pac.hunterMask=(uint8_t)req->getParam("hunters")->value().toInt();
        if (req->hasParam("prey"))
            g_pac.preyMask  =(uint8_t)req->getParam("prey")->value().toInt();
        if (req->hasParam("statics"))
            g_pac.staticMask=(uint8_t)req->getParam("statics")->value().toInt();
        if (req->hasParam("text")) {
            String t=req->getParam("text")->value();
            strncpy(g_pac.interText,t.c_str(),32); g_pac.interText[32]='\0';
            g_pac.interText[sizeof(g_pac.interText)-1]='\0';
        }
        savePacSettings();
        g_initialized=false;
        req->send(200,"text/plain","OK");
    });

    server->on("/pacman/ui", HTTP_GET, [](AsyncWebServerRequest* req) {
        String html=R"html(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pac-Man Settings</title>
<link rel="stylesheet" href="/style.css">
<style>
  table{border-collapse:collapse;width:100%;margin-bottom:12px}
  th,td{padding:5px 8px;text-align:left;border-bottom:1px solid #333;vertical-align:middle}
  th{color:#aaa;font-size:.85em;text-transform:uppercase;letter-spacing:.05em}
  .score-in{width:55px;background:#1a1a2e;color:#fff;border:1px solid #444;
            padding:2px 4px;border-radius:3px;text-transform:uppercase}
  .msg-in{width:100%;max-width:320px;background:#1a1a2e;color:#fff;
          border:1px solid #61dafb;padding:6px 8px;border-radius:4px;font-size:1em}
  .role-sel{background:#1a1a2e;color:#fff;border:1px solid #444;
            padding:2px 4px;border-radius:3px;font-size:.85em}
  .role-hunter{color:#e74c3c}
  .role-prey  {color:#3498db}
  .role-static{color:#2ecc71}
  .role-player{color:#f39c12}
  .role-flicker{color:#9b59b6}
  .role-unused{color:#666}
  .section{background:#282c34;border-radius:6px;padding:12px;margin-bottom:12px}
  .section h3{margin:0 0 10px;color:#61dafb;font-size:1em}
  button.save-btn{background:#61dafb;color:#282c34;border:none;padding:8px 24px;
                  border-radius:4px;font-size:1em;font-weight:bold;cursor:pointer}
  button.save-btn:hover{background:#4fa8d3}
</style>
</head><body>
<h2>&#x1F47E; Pac-Man Chase Settings</h2>
<div id="ui">Loading...</div>
<br><button class="save-btn" onclick="save()">Save Settings</button>
<script>
const ROLES=['hunter','prey','static','flicker','unused','player'];
const roleClass={hunter:'role-hunter',prey:'role-prey',static:'role-static',
                 flicker:'role-flicker',unused:'role-unused',player:'role-player'};

async function load(){
  const r=await fetch('/pacman/data');const d=await r.json();

  // Flatten all characters into one list
  const chars=[
    {name:'pac-man',   role:'player', idx:-1,  on:true,  score:'', cls:'hm', fixed:true},
    ...d.hunters.map(h=>({name:h.name,role:'hunter', idx:h.idx,on:h.on,score:h.score,cls:'hm',fixed:false})),
    ...d.prey   .map(p=>({name:p.name,role:'prey',   idx:p.idx,on:p.on,score:p.score,cls:'pm',fixed:false})),
    ...d.statics.map(s=>({name:s.name,role:'static', idx:s.idx,on:s.on,score:s.score,cls:'sm',fixed:false})),
  ];

  const roleOpts=(cur,fixed)=>fixed
    ? `<span class="${roleClass[cur]}">${cur}</span>`
    : ROLES.map(r=>`<option value="${r}" ${r===cur?'selected':''}>${r}</option>`).join('');

  document.getElementById('ui').innerHTML=`
    <div class="section">
      <h3>&#x1F4AC; Inter-chase message</h3>
      <small style="color:#aaa">Shown between chases — READY! appears every 6th chase regardless</small><br><br>
      <input class="msg-in" type="text" id="text" maxlength="32" value="${d.text}"
             oninput="this.value=this.value.toUpperCase()" placeholder="GO GO GO">
    </div>

    <div class="section">
      <h3>&#x1F3AE; Chase mode</h3>
      <label>Mode:&nbsp;<select id="mode">
        <option value="0" ${d.mode==0?'selected':''}>Pac-Man chases prey</option>
        <option value="1" ${d.mode==1?'selected':''}>Hunters chase Pac-Man</option>
      </select></label>&nbsp;&nbsp;
      <label>Count:&nbsp;<input type="number" id="nchasers" min="1" max="4" value="${d.nchasers}"
             style="width:44px;background:#1a1a2e;color:#fff;border:1px solid #444;border-radius:3px;padding:2px 4px"></label>&nbsp;&nbsp;
      <label><input type="checkbox" id="auto" ${d.autoswitch?'checked':''}>&nbsp;Auto-alternate</label>
    </div>

    <div class="section">
      <h3>&#x1F464; Characters</h3>
      <table>
        <tr><th>Name</th><th>Behavior</th><th>Enabled</th><th>Score</th></tr>
        ${chars.map(c=>`
        <tr>
          <td style="font-family:monospace">${c.name}</td>
          <td>${c.fixed
               ? `<span class="${roleClass[c.role]}">${c.role}</span>`
               : `<select class="role-sel" data-name="${c.name}" data-idx="${c.idx}" data-orig="${c.role}">
                    ${ROLES.map(ro=>`<option value="${ro}" ${ro===c.role?'selected':''}>${ro}</option>`).join('')}
                  </select>`}
          </td>
          <td>${c.fixed
               ? '&#x2713;'
               : `<input type="checkbox" class="${c.cls}" data-i="${c.idx}" ${c.on?'checked':''}>`}
          </td>
          <td>${c.fixed
               ? '—'
               : `<input class="score-in" type="text" maxlength="6" value="${c.score}">`}
          </td>
        </tr>`).join('')}
      </table>
      <small style="color:#666">Score text changes require sprites.py import + reflash.<br>
      Role changes are display-only here — use sprites.py to make permanent.</small>
    </div>`;
}

async function save(){
  const txt=document.getElementById('text').value.toUpperCase();
  let hm=0,pm=0,sm=0;
  document.querySelectorAll('.hm').forEach(c=>{if(c.checked)hm|=(1<<+c.dataset.i);});
  document.querySelectorAll('.pm').forEach(c=>{if(c.checked)pm|=(1<<+c.dataset.i);});
  document.querySelectorAll('.sm').forEach(c=>{if(c.checked)sm|=(1<<+c.dataset.i);});
  const r=await fetch('/pacman/set?'+new URLSearchParams({
    mode:document.getElementById('mode').value,
    nchasers:document.getElementById('nchasers').value,
    autoswitch:document.getElementById('auto').checked?'1':'0',
    hunters:hm,prey:pm,statics:sm,text:txt
  }));
  if(r.ok) alert('Saved!');
  else alert('Save failed.');
}
load();
</script>
</body></html>)html";
        req->send(200,"text/html",html);
    });
}
