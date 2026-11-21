#import <Cocoa/Cocoa.h>
#import "branding.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include <time.h>
#include "nacre_core.h"
#include "nacre_session.h"
#include "gb_input.h"
#include "../desktop/pad.h"
#include "../desktop/audio_out.h"

/* The Mac window runs every game through the system-agnostic core interface: a
 * NacreSession picks the core, keeps saves and states beside the content, and the
 * shared AudioOut plays whatever the core produces. Nothing here knows a system's
 * own header; per-system wording comes from the look table below. */

static NSString *snapshotPath;
static NSString *launchPath;
static double snapshotSeconds = 0.2;
static int snapshotStatus;

/* Clock systems choose a CPU rate; the others a multiple of real time. */
static const unsigned CLOCK_HZ[3] = {300, 600, 1000};
static const unsigned SPEED_PERCENT[3] = {50, 100, 200};
static const unsigned FAST_PERCENT = 400; /* while fast-forward is held */
static const unsigned long long MAX_CONTENT = 256ull << 20; /* larger content must be a NEEDS_PATH core's */
static const unsigned INKS[3] = {0xC3DCC7, 0xEBD8AF, 0xE8E9DF};

/* What the window says about each system it knows by name. Anything else gets the
 * neutral defaults: real-time speed and no refresh note. */
typedef struct {
    const char *id;
    BOOL clockSpeed; /* speed is the "cpu_hz" option */
    const char *rate;
} SystemLook;
static const SystemLook LOOKS_BY_SYSTEM[] = {
    {"chip8", YES, "60 HZ TIMERS"},
    {"gb", NO, "59.7 HZ FRAMES"},
    {"nds", NO, "59.8 HZ FRAMES"},
};
static const SystemLook DEFAULT_LOOK = {"", NO, ""};

static const SystemLook *look_for(const NacreCore *core) {
    if (core)
        for (size_t i = 0; i < sizeof(LOOKS_BY_SYSTEM) / sizeof(LOOKS_BY_SYSTEM[0]); i++)
            if (strcmp(LOOKS_BY_SYSTEM[i].id, core->id) == 0) return &LOOKS_BY_SYSTEM[i];
    return &DEFAULT_LOOK;
}

/* ---- The session's file access: UTF-8 paths, atomic writes. ---- */

static NacreIoResult io_read(void *userdata, const char *path, size_t max_size, uint8_t **data,
                             size_t *size) {
    (void)userdata;
    NSString *file = [NSString stringWithUTF8String:path];
    if (!file) return NACRE_IO_FAILED;
    NSFileManager *files = NSFileManager.defaultManager;
    NSDictionary *attributes = [files attributesOfItemAtPath:file error:NULL];
    if (!attributes) return [files fileExistsAtPath:file] ? NACRE_IO_FAILED : NACRE_IO_MISSING;
    if (attributes.fileSize == 0 || attributes.fileSize > max_size) return NACRE_IO_FAILED;
    NSData *bytes = [NSData dataWithContentsOfFile:file options:0 error:NULL];
    if (!bytes || bytes.length == 0 || bytes.length > max_size) return NACRE_IO_FAILED;
    *data = malloc(bytes.length);
    if (!*data) return NACRE_IO_FAILED;
    memcpy(*data, bytes.bytes, bytes.length);
    *size = bytes.length;
    return NACRE_IO_OK;
}

static int io_write(void *userdata, const char *path, const uint8_t *data, size_t size) {
    (void)userdata;
    NSString *file = [NSString stringWithUTF8String:path];
    if (!file || size == 0) return 0;
    /* Atomic: written to a temporary file and renamed, so a crash never tears it. */
    return [[NSData dataWithBytes:data length:size] writeToFile:file options:NSDataWritingAtomic
                                                          error:NULL] ? 1 : 0;
}

static int64_t io_now(void *userdata) {
    (void)userdata;
    return (int64_t)time(NULL);
}

static void host_log(void *userdata, NacreLogLevel level, const char *message) {
    (void)userdata;
    (void)level;
    NSLog(@"Nacre: %s", message);
}

/* "rom is too large" -> "Rom is too large." */
static NSString *sentence(const char *value) {
    NSString *out = value ? [NSString stringWithUTF8String:value] : @"";
    if (!out.length) return out;
    out = [[out substringToIndex:1].uppercaseString stringByAppendingString:[out substringFromIndex:1]];
    unichar last = [out characterAtIndex:out.length - 1];
    if (last != '.' && last != '!' && last != '?') out = [out stringByAppendingString:@"."];
    return out;
}

/* ---- Drawing helpers ---- */

static NSColor *color(unsigned rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
        green:((rgb >> 8) & 255) / 255.0 blue:(rgb & 255) / 255.0 alpha:1];
}
static void fill(NSRect rect, unsigned rgb, CGFloat radius) {
    [color(rgb) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:rect xRadius:radius yRadius:radius] fill];
}
static void text_aligned(NSString *value, NSRect rect, CGFloat size, NSFontWeight weight,
                         unsigned rgb, BOOL mono, NSTextAlignment alignment) {
    NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
    style.lineBreakMode = NSLineBreakByTruncatingTail;
    style.alignment = alignment;
    NSFont *font = mono ? [NSFont monospacedSystemFontOfSize:size weight:weight]
                        : [NSFont systemFontOfSize:size weight:weight];
    [value drawInRect:rect withAttributes:@{ NSFontAttributeName:font,
        NSForegroundColorAttributeName:color(rgb), NSParagraphStyleAttributeName:style }];
}
static void text(NSString *value, NSRect rect, CGFloat size, NSFontWeight weight,
                 unsigned rgb, BOOL mono) {
    text_aligned(value, rect, size, weight, rgb, mono, NSTextAlignmentNatural);
}
static void keycap(CGFloat x, CGFloat y, CGFloat w, NSString *label, BOOL held, CGFloat size) {
    fill(NSMakeRect(x, y + 2, w, 31), held ? 0x416452 : 0xD9DAD1, 6);
    fill(NSMakeRect(x, y, w, 30), held ? 0x527A64 : 0xF6F5F0, 6);
    text_aligned(label, NSMakeRect(x, y + (size < 12 ? 8 : 5), w, 22), size, NSFontWeightMedium,
                 held ? 0xFFFFFF : 0x596259, YES, NSTextAlignmentCenter);
}
/* Multi-shade screens run from the palette's ink (shade 0, the lightest) down to a
 * deep tone just above the well, so an LCD reads as the same phosphor as CHIP-8
 * pixels rather than a green screen pasted into the window. */
static unsigned shade_rgb(NSInteger palette, unsigned shade, unsigned levels) {
    const unsigned dark = 0x1E2A25;
    unsigned ink = INKS[palette], out = 0;
    unsigned steps = levels > 1 ? levels - 1 : 1;
    if (shade > steps) shade = steps;
    for (int channel = 0; channel < 3; channel++) {
        int shift = channel * 8;
        int a = (int)((ink >> shift) & 0xFF), b = (int)((dark >> shift) & 0xFF);
        out |= (unsigned)(a + (b - a) * (int)shade / (int)steps) << shift;
    }
    return out;
}

/* ---- Keys ---- */

static int chip_key(unsigned short code) {
    /* A 4x4 keypad on physical ANSI positions, independent of keyboard layout. */
    switch (code) {
        case 18:return 1; case 19:return 2; case 20:return 3; case 21:return 12;
        case 12:return 4; case 13:return 5; case 14:return 6; case 15:return 13;
        case 0:return 7; case 1:return 8; case 2:return 9; case 3:return 14;
        case 6:return 10; case 7:return 0; case 8:return 11; case 9:return 15;
        default:return -1;
    }
}
/* Pad keys are bound by physical key code (the same on any layout): arrows and
 * WASD for the D-pad, X/K for A, Z/J for B, Delete or the right Shift for Select,
 * Return or Enter for Start, C/I for X, V/U for Y, Q and E for L and R (DS only);
 * Tab fast-forwards, F5 and F7 save and load, F6 changes the save slot. A keypad system takes its 4x4 block of keys first. */
static const int DEFAULT_KEYS[GBI_COUNT][GBI_KEYS_PER_ACTION] = {
    {124, 2}, {123, 0}, {126, 13}, {125, 1}, /* Right, Left, Up, Down */
    {7, 40}, {6, 38},                        /* A, B */
    {51, 60}, {36, 76},                      /* Select, Start */
    {8, 34}, {9, 32}, {12, -1}, {14, -1},    /* X, Y, L, R */
    {48, -1}, {96, -1}, {98, -1}, {97, -1},  /* fast-forward, save, load, next slot */
};
static NSString *key_name(int code) {
    static const char *const names[128] = {
        [0]="A", [1]="S", [2]="D", [3]="F", [4]="H", [5]="G", [6]="Z", [7]="X", [8]="C", [9]="V",
        [11]="B", [12]="Q", [13]="W", [14]="E", [15]="R", [16]="Y", [17]="T",
        [18]="1", [19]="2", [20]="3", [21]="4", [22]="6", [23]="5", [24]="=", [25]="9", [26]="7",
        [27]="-", [28]="8", [29]="0", [30]="]", [31]="O", [32]="U", [33]="[", [34]="I", [35]="P",
        [36]="Return", [37]="L", [38]="J", [39]="'", [40]="K", [41]=";", [42]="\\", [43]=",",
        [44]="/", [45]="N", [46]="M", [47]=".", [48]="Tab", [49]="Space", [50]="`", [51]="Delete",
        [53]="Esc", [56]="Shift", [60]="Right Shift",
        [65]="Keypad .", [67]="Keypad *", [69]="Keypad +", [71]="Clear", [75]="Keypad /",
        [76]="Enter", [78]="Keypad -", [81]="Keypad =", [82]="Keypad 0", [83]="Keypad 1",
        [84]="Keypad 2", [85]="Keypad 3", [86]="Keypad 4", [87]="Keypad 5", [88]="Keypad 6",
        [89]="Keypad 7", [91]="Keypad 8", [92]="Keypad 9",
        [96]="F5", [97]="F6", [98]="F7", [99]="F3", [100]="F8", [101]="F9", [103]="F11",
        [105]="F13", [109]="F10", [111]="F12", [115]="Home", [116]="Page Up",
        [117]="Forward Delete", [118]="F4", [119]="End", [120]="F2", [121]="Page Down",
        [122]="F1", [123]="Left Arrow", [124]="Right Arrow", [125]="Down Arrow", [126]="Up Arrow",
    };
    if (code < 0) return @"—";
    if (code < 128 && names[code]) return [NSString stringWithUTF8String:names[code]];
    return [NSString stringWithFormat:@"Key %d", code];
}
/* A compact label for a keycap. */
static NSString *short_key(int code) {
    switch (code) {
        case 123: return @"←"; case 124: return @"→"; case 125: return @"↓"; case 126: return @"↑";
        case 36: case 76: return @"↩"; case 51: return @"⌫"; case 48: return @"⇥";
        case 49: return @"␣"; case 56: return @"⇧"; case 60: return @"R⇧";
        default: return key_name(code).uppercaseString;
    }
}
static int first_key(const GbBindings *bindings, int action) {
    return bindings->key[action][0] >= 0 ? bindings->key[action][0] : bindings->key[action][1];
}
/* Keys that already mean something else, so they can't be bound: Esc, Space (pause),
 * M (mute), F2 (restart) and the modifier keys. */
static BOOL key_reserved(int code) {
    switch (code) {
        case 53: case 49: case 46: case 120:
        case 54: case 55: case 57: case 58: case 59: case 61: case 62: case 63:
            return YES;
        default:
            return code < 0 || code > 127;
    }
}

@class NacreController;
@interface NacreView : NSView
@property(nonatomic, weak) NacreController *controller;
@property(nonatomic) BOOL dropActive;
@end
@interface NacreController : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate> {
@public
    NacreSession *session;
    AudioOut *audio;
    uint8_t keypadHeld[16];   /* keypad keys the keyboard holds */
    unsigned kbHeld;          /* GBI_BIT pad actions the keyboard holds */
    unsigned padHeld, padSeen; /* controller actions now / at the last poll */
    uint32_t padButtons;      /* NACRE_PAD bits the core was last given */
    BOOL ffKey, ffOn;         /* fast-forward key held; fast-forward running */
    BOOL pointerDown;         /* the mouse is pressed on a touchscreen core */
    NSRect shownRect;         /* where the last frame was drawn, in view points */
    unsigned shownWidth, shownHeight;
    int appliedRunning;       /* what the session was last told, -1 for nothing yet */
    unsigned appliedSpeed;
    GbBindings keys;
    CGContextRef frameContext;
    unsigned contextWidth, contextHeight;
}
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NacreView *view;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSURL *romURL;     /* nil for built-in content */
@property(nonatomic, strong) NSURL *pendingURL;
@property(nonatomic) BOOL bundled;
@property(nonatomic) NSInteger clockChoice;     /* index into CLOCK_HZ */
@property(nonatomic) NSInteger speedChoice;     /* index into SPEED_PERCENT */
@property(nonatomic) BOOL userPaused;
@property(nonatomic) BOOL focusPaused;
@property(nonatomic) BOOL dialogOpen;
@property(nonatomic) BOOL muted;
@property(nonatomic) NSInteger palette;
@property(nonatomic) uint64_t previous;
@property(nonatomic) NSInteger slot; /* save-state slot, 0..NACRE_STATE_SLOTS-1 */
@property(nonatomic, strong) NSString *statusText;
@property(nonatomic) uint64_t statusUntil;
@property(nonatomic, strong) NSWindow *controlsSheet;
@property(nonatomic, strong) NSButton *openButton;
@property(nonatomic, strong) NSButton *pauseButton;
@property(nonatomic, strong) NSButton *resetButton;
@property(nonatomic, strong) NSButton *soundButton;
@property(nonatomic, strong) NSPopUpButton *paletteButton;
@property(nonatomic, strong) NSPopUpButton *speedButton;
- (const NacreCore *)core;
- (void *)instance;
- (BOOL)ready;
- (const char *)faultText;
- (BOOL)hasKeypad;
- (BOOL)hasPad;
- (BOOL)statesReady;
- (BOOL)audioReady;
- (const NacreVideoFrame *)frame;
- (BOOL)paused;
- (BOOL)key:(unsigned short)code down:(BOOL)down repeat:(BOOL)repeat;
- (void)openROM:(id)sender;
- (void)togglePause:(id)sender;
- (void)resetROM:(id)sender;
- (void)toggleSound:(id)sender;
- (void)changePalette:(id)sender;
- (void)changeSpeed:(id)sender;
- (void)loadURL:(NSURL *)url;
- (void)releaseKeys;
- (void)updateControls;
- (void)savePrefs;
- (void)showControls:(id)sender;
- (void)closeControls;
- (void)drawFrameInWell:(NSRect)well;
- (void)pointerAt:(NSPoint)point pressed:(BOOL)pressed;
@end

/* The rebinding sheet: every action with its two keys. */
@interface NacreControlsView : NSView
@property(nonatomic, weak) NacreController *controller;
@property(nonatomic) int action, cellSlot;
@property(nonatomic) BOOL capturing;
@property(nonatomic, strong) NSString *note;
@end

@implementation NacreView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)event {
    if (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                               NSEventModifierFlagOption)) return;
    if (!event.isARepeat) {
        if (event.keyCode == 49) { [self.controller togglePause:nil]; return; }
        if (event.keyCode == 46) { [self.controller toggleSound:nil]; return; }
        if (event.keyCode == 120) { [self.controller resetROM:nil]; return; }
        if (event.keyCode == 53 && self.window.styleMask & NSWindowStyleMaskFullScreen) {
            [self.window toggleFullScreen:nil]; return;
        }
    }
    if ([self.controller key:event.keyCode down:YES repeat:event.isARepeat]) self.needsDisplay = YES;
}
- (void)keyUp:(NSEvent *)event {
    if ([self.controller key:event.keyCode down:NO repeat:NO]) self.needsDisplay = YES;
}
- (void)flagsChanged:(NSEvent *)event {
    /* Shift is a modifier, so it arrives here rather than in keyDown. The device-dependent
     * bits say which one moved: 0x2 is the left Shift, 0x4 the right. */
    unsigned short code = event.keyCode;
    if (code == 56 || code == 60) {
        BOOL down = (event.modifierFlags & (code == 60 ? 0x4 : 0x2)) != 0;
        if ([self.controller key:code down:down repeat:NO]) self.needsDisplay = YES;
    }
    [super flagsChanged:event];
}
- (void)mouseDown:(NSEvent *)event {
    [self.window makeFirstResponder:self];
    [self.controller pointerAt:[self convertPoint:event.locationInWindow fromView:nil] pressed:YES];
}
- (void)mouseDragged:(NSEvent *)event {
    [self.controller pointerAt:[self convertPoint:event.locationInWindow fromView:nil] pressed:YES];
}
- (void)mouseUp:(NSEvent *)event {
    [self.controller pointerAt:[self convertPoint:event.locationInWindow fromView:nil] pressed:NO];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    BOOL hasFile = [sender.draggingPasteboard canReadObjectForClasses:@[NSURL.class]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}];
    self.dropActive = hasFile; self.needsDisplay = YES;
    return hasFile ? NSDragOperationCopy : NSDragOperationNone;
}
- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender; self.dropActive = NO; self.needsDisplay = YES;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL *> *urls = [sender.draggingPasteboard readObjectsForClasses:@[NSURL.class]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}];
    self.dropActive = NO;
    if (urls.count != 1) { self.needsDisplay = YES; return NO; }
    [self.controller loadURL:urls.firstObject]; return YES;
}
- (void)layout {
    [super layout];
    CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
    NacreController *c = self.controller;
    c.openButton.frame = NSMakeRect(24, 148, 174, 34);
    c.speedButton.frame = NSMakeRect(22, 274, 178, 30);
    c.paletteButton.frame = NSMakeRect(22, 350, 178, 30);
    c.pauseButton.frame = NSMakeRect(252, h - 88, 116, 34);
    c.resetButton.frame = NSMakeRect(380, h - 88, 90, 34);
    c.soundButton.frame = NSMakeRect(w - 164, h - 88, 132, 34);
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NacreController *c = self.controller;
    CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
    const NacreCore *core = [c core];
    const char *halted = [c faultText];
    fill(self.bounds, 0xF5F3EE, 0);
    fill(NSMakeRect(0, 0, 222, h), 0xEBE9E2, 0);
    fill(NSMakeRect(221, 0, 1, h), 0xDAD8D0, 0);

    /* A small drawn shell mark; no external images or font dependencies. */
    [color(0x587668) setStroke];
    for (int i = 0; i < 4; i++) {
        NSBezierPath *arc = [NSBezierPath bezierPath]; arc.lineWidth = 1.5;
        [arc moveToPoint:NSMakePoint(26 + i * 4, 29)];
        [arc curveToPoint:NSMakePoint(51, 54 - i * 4)
            controlPoint1:NSMakePoint(58, 22 + i * 4)
            controlPoint2:NSMakePoint(65 - i * 3, 48)];
        [arc stroke];
    }
    NSFont *wordmark = [NSFont fontWithName:@"Georgia" size:31];
    if (!wordmark) wordmark = [NSFont systemFontOfSize:31];
    [@"nacre" drawAtPoint:NSMakePoint(66, 22) withAttributes:@{
        NSFontAttributeName:wordmark, NSForegroundColorAttributeName:color(0x293D34)}];
    text(@"SESSION", NSMakeRect(26, 114, 174, 18), 10, NSFontWeightSemibold, 0x83867D, YES);
    text(@"Drop a ROM anywhere to begin.", NSMakeRect(26, 192, 178, 32), 11,
         NSFontWeightRegular, 0x747B72, NO);
    fill(NSMakeRect(26, 232, 170, 1), 0xD8D8CF, 0);
    text(@"Execution speed", NSMakeRect(26, 249, 174, 20), 12, NSFontWeightMedium, 0x4A544B, NO);
    text(@"Display", NSMakeRect(26, 325, 174, 20), 12, NSFontWeightMedium, 0x4A544B, NO);
    text(@"KEYBOARD", NSMakeRect(26, 414, 174, 18), 10, NSFontWeightSemibold, 0x83867D, YES);
    if ([c hasKeypad]) {
        const char *host = "1234QWERASDFZXCV";
        const int values[16] = {1,2,3,12,4,5,6,13,7,8,9,14,10,0,11,15};
        for (int i = 0; i < 16; i++)
            keycap(26 + (i % 4) * 44, 444 + (i / 4) * 39, 36,
                   [NSString stringWithFormat:@"%c", host[i]], c->keypadHeld[values[i]] != 0, 12);
    } else {
        /* A D-pad cross with the face buttons beside it, then Select and Start, each
         * labelled with the keyboard key that presses it; Select and Start also carry
         * the system's own name for them. A DS adds X, Y and the shoulders. */
        typedef struct { int action; NacrePadButton button; int col, row, cols; CGFloat size; } Cap;
        static const Cap twoButtons[] = {
            {GBI_UP, NACRE_PAD_UP, 1, 0, 1, 15},       {GBI_LEFT, NACRE_PAD_LEFT, 0, 1, 1, 15},
            {GBI_RIGHT, NACRE_PAD_RIGHT, 2, 1, 1, 15}, {GBI_DOWN, NACRE_PAD_DOWN, 1, 2, 1, 15},
            {GBI_A, NACRE_PAD_EAST, 3, 0, 1, 12},      {GBI_B, NACRE_PAD_SOUTH, 3, 1, 1, 12},
            {GBI_SELECT, NACRE_PAD_SELECT, 0, 3, 2, 9}, {GBI_START, NACRE_PAD_START, 2, 3, 2, 9},
        };
        static const Cap fourButtons[] = {
            {GBI_L, NACRE_PAD_L1, 0, 0, 1, 12},        {GBI_UP, NACRE_PAD_UP, 1, 0, 1, 15},
            {GBI_R, NACRE_PAD_R1, 2, 0, 1, 12},        {GBI_X, NACRE_PAD_NORTH, 3, 0, 1, 12},
            {GBI_LEFT, NACRE_PAD_LEFT, 0, 1, 1, 15},   {GBI_RIGHT, NACRE_PAD_RIGHT, 2, 1, 1, 15},
            {GBI_A, NACRE_PAD_EAST, 3, 1, 1, 12},      {GBI_DOWN, NACRE_PAD_DOWN, 1, 2, 1, 15},
            {GBI_Y, NACRE_PAD_WEST, 2, 2, 1, 12},      {GBI_B, NACRE_PAD_SOUTH, 3, 2, 1, 12},
            {GBI_SELECT, NACRE_PAD_SELECT, 0, 3, 2, 9}, {GBI_START, NACRE_PAD_START, 2, 3, 2, 9},
        };
        BOOL four = core && core->pad_labels[NACRE_PAD_NORTH] && core->pad_labels[NACRE_PAD_WEST] &&
                    core->pad_labels[NACRE_PAD_L1] && core->pad_labels[NACRE_PAD_R1];
        const Cap *caps = four ? fourButtons : twoButtons;
        size_t capCount = four ? sizeof(fourButtons) / sizeof(fourButtons[0]) : sizeof(twoButtons) / sizeof(twoButtons[0]);
        for (size_t i = 0; i < capCount; i++) {
            NSString *label = short_key(first_key(&c->keys, caps[i].action));
            if (caps[i].button == NACRE_PAD_SELECT || caps[i].button == NACRE_PAD_START) {
                const char *named = core ? core->pad_labels[caps[i].button] : NULL;
                NSString *suffix = named ? [NSString stringWithUTF8String:named].uppercaseString
                                         : caps[i].button == NACRE_PAD_START ? @"START" : @"SELECT";
                label = [NSString stringWithFormat:@"%@ %@", label, suffix];
            }
            keycap(26 + caps[i].col * 44, 444 + caps[i].row * 39, caps[i].cols * 44 - 8, label,
                   (c->padButtons & NACRE_PAD_BIT(caps[i].button)) != 0, caps[i].size);
        }
    }
    CGFloat hintY = 602;
    if ([c statesReady]) {
        int fast = first_key(&c->keys, GBI_FAST_FORWARD);
        text(fast >= 0 ? [NSString stringWithFormat:@"Hold %@ to fast-forward", key_name(fast)]
                       : @"Fast-forward is unbound",
             NSMakeRect(26, hintY, 190, 18), 10, NSFontWeightRegular, 0x7F847A, NO);
        text([NSString stringWithFormat:@"%@ saves  ·  %@ loads  ·  slot %ld",
                 short_key(first_key(&c->keys, GBI_SAVE_STATE)),
                 short_key(first_key(&c->keys, GBI_LOAD_STATE)), (long)c.slot + 1],
             NSMakeRect(26, hintY + 16, 190, 18), 10, NSFontWeightRegular, 0x7F847A, NO);
        hintY += 32;
    }
    if (core && (core->flags & NACRE_CORE_HAS_POINTER)) {
        text(@"Click the lower screen to touch", NSMakeRect(26, hintY, 190, 18), 10,
             NSFontWeightRegular, 0x7F847A, NO);
        hintY += 16;
    }
    text(@"Space to pause  ·  M to mute", NSMakeRect(26, hintY, 190, 18), 10,
         NSFontWeightRegular, 0x7F847A, NO);
    text(@"Session ▸ Controls… to rebind", NSMakeRect(26, hintY + 16, 190, 18), 10,
         NSFontWeightRegular, 0x7F847A, NO);
    const NacreVideoFrame *frame = [c frame];
    if (core) {
        NSString *system = [NSString stringWithUTF8String:core->name].uppercaseString;
        if (frame) system = [system stringByAppendingFormat:@"  /  %u × %u", frame->width, frame->height];
        text(system, NSMakeRect(26, h - 36, 180, 18), 10, NSFontWeightMedium, 0x878C80, YES);
    }

    NSString *name = c.romURL ? c.romURL.lastPathComponent.stringByDeletingPathExtension : @"Your next little world";
    if (c.bundled) name = @"Welcome to Nacre";
    text(name, NSMakeRect(254, 27, w - 430, 32), 23, NSFontWeightSemibold, 0x293D34, NO);
    NSString *kind = @"";
    if (c.bundled) {
        kind = @"PRESS A KEYPAD KEY TO SAY HELLO";
    } else if (core) {
        const char *named = core->title ? core->title([c instance]) : NULL;
        kind = [NSString stringWithUTF8String:core->name].uppercaseString;
        kind = named ? [kind stringByAppendingFormat:@"  ·  %@", [NSString stringWithUTF8String:named]] : [kind stringByAppendingString:@" ROM"];
    }
    text(kind, NSMakeRect(255, 63, w - 400, 18), 10, NSFontWeightMedium, 0x7B8479, YES);
    BOOL paused = [c paused];
    BOOL fast = c->ffOn && !paused;
    fill(NSMakeRect(w - 133, 42, 6, 6), halted ? 0xB66C52 : paused ? 0xB09361 : 0x608C70, 3);
    text(halted ? @"STOPPED" : paused ? @"PAUSED" : fast ? @"FAST 4×" : @"RUNNING",
         NSMakeRect(w - 118, 37, 90, 20), 10, NSFontWeightMedium, 0x6C786C, YES);

    NSRect well = NSMakeRect(254, 111, w - 286, h - 233);
    fill(NSOffsetRect(well, 0, 3), 0xD7D8CE, 14);
    fill(well, self.dropActive ? 0x294C3D : 0x18211E, 14);
    [c drawFrameInWell:well];
    text(@"NACRE", NSMakeRect(well.origin.x + 18, NSMaxY(well) - 25, 100, 14), 8,
         NSFontWeightMedium, 0x64776B, YES);
    if (paused || self.dropActive || halted || fast) {
        NSString *badge = self.dropActive ? @"Drop to open ROM" : halted ? @"Execution stopped"
                        : paused ? @"Paused" : @"Fast-forward 4×";
        NSRect badgeRect = NSMakeRect(NSMidX(well) - 92, well.origin.y + 18, 184, 30);
        fill(badgeRect, 0x35483C, 15);
        NSMutableParagraphStyle *center = [NSMutableParagraphStyle new]; center.alignment = NSTextAlignmentCenter;
        [badge drawInRect:NSInsetRect(badgeRect, 0, 6) withAttributes:@{
            NSFontAttributeName:[NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName:color(0xDCE8DA), NSParagraphStyleAttributeName:center}];
    }
    NSString *note = nil;
    if (halted) note = sentence(halted);
    else if (c.statusText && SDL_GetTicksNS() < c.statusUntil) note = c.statusText;
    else if (core && ![c audioReady]) note = @"Audio unavailable";
    else if (c->session) {
        const char *saved = nacre_session_save_note(c->session);
        if (saved && *saved) note = [NSString stringWithUTF8String:saved];
    }
    if (note) text(note, NSMakeRect(254, h - 34, w - 450, 18), 10, NSFontWeightRegular, 0x7A8276, NO);
    text([NSString stringWithUTF8String:look_for(core)->rate], NSMakeRect(w - 146, h - 34, 120, 18), 9,
         NSFontWeightMedium, 0x87907F, YES);
}
@end

@implementation NacreControlsView {
    NSButton *restoreButton, *doneButton;
}
#define CONTROLS_ROW 27
static NSRect cell_rect(int row, int slot) {
    return NSMakeRect(slot ? 360 : 200, 104 + row * CONTROLS_ROW, 140, 23);
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    restoreButton = [NSButton buttonWithTitle:@"Restore defaults" target:self action:@selector(restore:)];
    restoreButton.frame = NSMakeRect(24, 640, 150, 30);
    doneButton = [NSButton buttonWithTitle:@"Done" target:self action:@selector(done:)];
    doneButton.frame = NSMakeRect(frame.size.width - 124, 640, 100, 30);
    [self addSubview:restoreButton]; [self addSubview:doneButton];
    return self;
}
- (void)restore:(id)sender {
    (void)sender;
    gbi_set_defaults(&self.controller->keys, DEFAULT_KEYS);
    [self.controller savePrefs];
    self.capturing = NO; self.note = @"Defaults restored.";
    [self.window makeFirstResponder:self];
    self.needsDisplay = YES; self.controller.view.needsDisplay = YES;
}
- (void)done:(id)sender { (void)sender; [self.controller closeControls]; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NacreController *c = self.controller;
    fill(self.bounds, 0xF5F3EE, 0);
    text(@"Controls", NSMakeRect(28, 22, 480, 24), 17, NSFontWeightSemibold, 0x293D34, NO);
    text(@"Click a key, then press the new one. Esc cancels; Delete clears.",
         NSMakeRect(28, 50, 490, 18), 11, NSFontWeightRegular, 0x747B72, NO);
    text(@"KEY 1", NSMakeRect(200, 82, 140, 14), 9, NSFontWeightSemibold, 0x83867D, YES);
    text(@"KEY 2", NSMakeRect(360, 82, 140, 14), 9, NSFontWeightSemibold, 0x83867D, YES);
    for (int row = 0; row < GBI_COUNT; row++) {
        CGFloat y = 104 + row * CONTROLS_ROW;
        if (row == GBI_BUTTONS) fill(NSMakeRect(28, y - 2, 472, 1), 0xD8D8CF, 0);
        text([NSString stringWithUTF8String:gbi_name((GbInput)row)], NSMakeRect(28, y + 3, 160, 18), 12,
             NSFontWeightMedium, 0x4A544B, NO);
        for (int slot = 0; slot < GBI_KEYS_PER_ACTION; slot++) {
            NSRect rect = cell_rect(row, slot);
            BOOL here = row == self.action && slot == self.cellSlot;
            fill(rect, here ? (self.capturing ? 0xCFE0D4 : 0xE0EAE2) : 0xEFEDE6, 6);
            if (here) {
                NSBezierPath *outline = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 0.5, 0.5)
                                                                        xRadius:6 yRadius:6];
                [color(0x587668) setStroke]; outline.lineWidth = 1.5; [outline stroke];
            }
            NSString *label = here && self.capturing ? @"Press a key…" : key_name(c->keys.key[row][slot]);
            text_aligned(label, NSInsetRect(rect, 6, 4), 12, NSFontWeightMedium,
                         here && self.capturing ? 0x355A43 : 0x3A463E, NO, NSTextAlignmentCenter);
        }
    }
    if (self.note) text(self.note, NSMakeRect(28, 540, 480, 16), 11, NSFontWeightMedium, 0xB66C52, NO);
    text(@"Controller: D-pad or left stick, face buttons by position, Start and Back.",
         NSMakeRect(28, 562, 490, 16), 10, NSFontWeightRegular, 0x7F847A, NO);
    text(@"Game Boy: right shoulder or either trigger fast-forwards; the left shoulder is the modifier.",
         NSMakeRect(28, 578, 490, 16), 10, NSFontWeightRegular, 0x7F847A, NO);
    text(@"DS: the shoulders are L and R; the right trigger fast-forwards, the left is the modifier.",
         NSMakeRect(28, 594, 490, 16), 10, NSFontWeightRegular, 0x7F847A, NO);
    text(@"Modifier held + Start, Back or the top button: save, load, next slot.",
         NSMakeRect(28, 610, 490, 16), 10, NSFontWeightRegular, 0x7F847A, NO);
}
- (void)mouseDown:(NSEvent *)event {
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    for (int row = 0; row < GBI_COUNT; row++) {
        for (int slot = 0; slot < GBI_KEYS_PER_ACTION; slot++) {
            if (NSPointInRect(point, cell_rect(row, slot))) {
                self.action = row; self.cellSlot = slot; self.capturing = YES; self.note = nil;
                [self.window makeFirstResponder:self];
                self.needsDisplay = YES;
                return;
            }
        }
    }
    self.capturing = NO; self.needsDisplay = YES;
}
/* The key to bind arrived. */
- (void)capture:(unsigned short)code {
    NacreController *c = self.controller;
    if (code == 53) {
        self.capturing = NO; self.note = nil;
    } else if (key_reserved(code)) {
        self.note = @"That key is reserved.";
    } else {
        int previous = gbi_assign(&c->keys, (GbInput)self.action, self.cellSlot, code);
        self.note = previous >= 0
            ? [NSString stringWithFormat:@"Taken from %s.", gbi_name((GbInput)previous)] : nil;
        self.capturing = NO;
        [c savePrefs];
        c.view.needsDisplay = YES;
    }
    self.needsDisplay = YES;
}
- (void)keyDown:(NSEvent *)event {
    if (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                               NSEventModifierFlagOption)) return;
    unsigned short code = event.keyCode;
    if (self.capturing) {
        if (!event.isARepeat) [self capture:code];
        return;
    }
    NacreController *c = self.controller;
    switch (code) {
        case 126: self.action = (self.action + GBI_COUNT - 1) % GBI_COUNT; break;
        case 125: self.action = (self.action + 1) % GBI_COUNT; break;
        case 123: case 124: case 48: self.cellSlot = (self.cellSlot + 1) % GBI_KEYS_PER_ACTION; break;
        case 36: case 76: self.capturing = YES; break;
        case 51: case 117:
            gbi_assign(&c->keys, (GbInput)self.action, self.cellSlot, GBI_UNBOUND);
            [c savePrefs]; c.view.needsDisplay = YES;
            break;
        case 53: [c closeControls]; return;
        default: return;
    }
    self.note = nil;
    self.needsDisplay = YES;
}
- (void)flagsChanged:(NSEvent *)event {
    unsigned short code = event.keyCode;
    if (self.capturing && (code == 56 || code == 60) &&
        (event.modifierFlags & (code == 60 ? 0x4 : 0x2))) {
        [self capture:code];
        return;
    }
    [super flagsChanged:event];
}
@end

@implementation NacreController

/* ---- What is running ---- */
- (const NacreCore *)core { return session ? nacre_session_core(session) : NULL; }
- (void *)instance { return session ? nacre_session_instance(session) : NULL; }
- (BOOL)ready { return [self core] != NULL; }
- (const char *)faultText { return session ? nacre_session_error(session) : NULL; }
- (BOOL)hasKeypad { const NacreCore *c = [self core]; return c && c->set_keypad && c->keypad_keys >= 16; }
- (BOOL)hasPad { const NacreCore *c = [self core]; return c && c->set_pad; }
- (BOOL)statesReady { return [self ready] && nacre_session_has_states(session); }
- (BOOL)audioReady { return audio_out_available(audio) != 0; }
- (const NacreVideoFrame *)frame {
    const NacreCore *c = [self core];
    return c && c->video ? c->video([self instance]) : NULL;
}
- (const SystemLook *)look { return look_for([self core]); }
- (BOOL)paused {
    return self.userPaused || self.focusPaused || self.dialogOpen || [self faultText] != NULL;
}
- (unsigned)speedPercent {
    if (ffOn) return FAST_PERCENT;
    return [self look]->clockSpeed ? 100u : SPEED_PERCENT[self.speedChoice];
}
/* Tells the session what the window wants: running or not, and how fast. */
- (void)syncSession {
    if (!session) return;
    int run = [self paused] ? 0 : 1;
    if (run != appliedRunning) {
        nacre_session_set_running(session, run);
        appliedRunning = run;
    }
    unsigned percent = [self speedPercent];
    if (percent != appliedSpeed) {
        nacre_session_set_speed(session, percent);
        appliedSpeed = percent;
    }
}
- (void)pumpAudio {
    audio_out_pump(audio, [self core], [self instance], [self speedPercent],
                   !self.muted && ![self paused], ffOn);
}
- (void)syncAll { [self syncSession]; [self pumpAudio]; }

/* ---- Input ---- */
/* The core sees a button held when the keyboard or a controller holds it. */
- (void)applyButtons {
    unsigned actions = kbHeld | ([self paused] ? 0u : padHeld);
    uint32_t buttons = gbi_pad_buttons(actions);
    if (buttons == padButtons) return;
    padButtons = buttons;
    if ([self hasPad]) [self core]->set_pad([self instance], 0, buttons);
}
- (void)setKeypadKey:(int)key held:(BOOL)held {
    if (key < 0 || key >= 16) return;
    keypadHeld[key] = held ? 1 : 0;
    if ([self hasKeypad]) [self core]->set_keypad([self instance], (unsigned)key, held ? 1 : 0);
}
- (void)releaseKeys {
    BOOL keypad = [self hasKeypad];
    for (int key = 0; key < 16; key++) {
        if (keypadHeld[key] && keypad) [self core]->set_keypad([self instance], (unsigned)key, 0);
        keypadHeld[key] = 0;
    }
    kbHeld = 0; padHeld = 0; ffKey = NO; padButtons = 0;
    if ([self hasPad]) [self core]->set_pad([self instance], 0, 0);
    if (pointerDown && [self core] && [self core]->set_pointer) [self core]->set_pointer([self instance], 0, -1, -1, 0);
    pointerDown = NO;
}
/* A click on a touchscreen core goes to it in the pixels of its frame; off the
 * frame is -1, -1. */
- (void)pointerAt:(NSPoint)point pressed:(BOOL)pressed {
    const NacreCore *core = [self core];
    if (!core || !(core->flags & NACRE_CORE_HAS_POINTER)) return;
    if (pressed && !pointerDown && ([self paused] || self.dialogOpen)) return;
    if (!pressed && !pointerDown) return;
    int fx = -1, fy = -1;
    if (shownRect.size.width > 0 && shownRect.size.height > 0 && NSPointInRect(point, shownRect)) {
        fx = (int)((point.x - shownRect.origin.x) / shownRect.size.width * shownWidth);
        fy = (int)((point.y - shownRect.origin.y) / shownRect.size.height * shownHeight);
    }
    pointerDown = pressed;
    core->set_pointer([self instance], 0, fx, fy, pressed ? 1 : 0);
}
/* A bound key was pressed. Pad buttons reach the core; fast-forward and the state
 * keys work for any game. */
- (void)actionPress:(int)action repeat:(BOOL)repeat {
    if (action < GBI_BUTTONS) {
        if (![self paused] && !repeat) {
            kbHeld |= GBI_BIT(action);
            [self applyButtons];
        }
    } else if (action == GBI_FAST_FORWARD) {
        ffKey = YES;
    } else if (!repeat) {
        if (action == GBI_SAVE_STATE) [self saveState];
        else if (action == GBI_LOAD_STATE) [self loadState];
        else if (action == GBI_NEXT_SLOT) [self chooseSlotNumber:(self.slot + 1) % NACRE_STATE_SLOTS];
    }
}
- (void)actionRelease:(int)action {
    if (action < GBI_BUTTONS) {
        kbHeld &= ~GBI_BIT(action);
        [self applyButtons];
    } else if (action == GBI_FAST_FORWARD) {
        ffKey = NO;
    }
}
/* A physical key went down or up. A keypad system takes its 4x4 block of keys
 * before any binding. Returns whether the key meant something. */
- (BOOL)key:(unsigned short)code down:(BOOL)down repeat:(BOOL)repeat {
    int pad = [self hasKeypad] ? chip_key(code) : -1;
    if (pad >= 0) {
        if (!down) [self setKeypadKey:pad held:NO];
        else if (![self paused] && !repeat) [self setKeypadKey:pad held:YES];
        return YES;
    }
    int action = gbi_lookup(&keys, code);
    if (action >= 0 && action < GBI_BUTTONS && ![self hasPad]) action = -1; /* nothing to press */
    if (action < 0) return NO;
    if (down) [self actionPress:action repeat:repeat];
    else [self actionRelease:action];
    return YES;
}
/* Reads the controllers: held buttons feed the core, and the state hotkeys fire once
 * per press. Only the focused window listens, so a game in the background isn't played. */
- (void)pollPad {
    unsigned held = pad_poll(gbi_labeled_buttons([self core]));
    const char *note = pad_take_message();
    if (note) [self setStatus:[NSString stringWithUTF8String:note]];
    BOOL listening = [self ready] && !self.dialogOpen && self.window.isKeyWindow;
    if (!listening) held = 0;
    unsigned fresh = held & ~padSeen;
    padSeen = held; padHeld = held;
    [self applyButtons];
    if (fresh & GBI_BIT(GBI_SAVE_STATE)) [self saveState];
    if (fresh & GBI_BIT(GBI_LOAD_STATE)) [self loadState];
    if (fresh & GBI_BIT(GBI_NEXT_SLOT)) [self chooseSlotNumber:(self.slot + 1) % NACRE_STATE_SLOTS];
}

/* ---- Preferences and save states ---- */
- (void)loadPrefs {
    gbi_set_defaults(&keys, DEFAULT_KEYS);
    self.slot = 0;
    self.clockChoice = 1; self.speedChoice = 1; self.palette = 0; self.muted = NO;
    if (snapshotPath) return; /* the self-check always runs on the defaults */
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSString *saved = [defaults stringForKey:@"gbBindings"];
    if (saved) gbi_parse(&keys, saved.UTF8String); /* junk leaves the defaults in place */
    NSInteger slot = [defaults integerForKey:@"gbSlot"];
    if (slot >= 0 && slot < NACRE_STATE_SLOTS) self.slot = slot;
    for (NSInteger i = 0; i < 3; i++) {
        if ([defaults integerForKey:@"cpuHz"] == (NSInteger)CLOCK_HZ[i]) self.clockChoice = i;
        if ([defaults integerForKey:@"gbSpeed"] == (NSInteger)SPEED_PERCENT[i]) self.speedChoice = i;
    }
    NSInteger palette = [defaults integerForKey:@"palette"];
    if (palette >= 0 && palette <= 2) self.palette = palette;
    self.muted = [defaults boolForKey:@"muted"];
}
- (void)savePrefs {
    if (snapshotPath) return;
    char buffer[512];
    if (gbi_serialize(&keys, buffer, sizeof(buffer)) == 0) return;
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    [defaults setObject:[NSString stringWithUTF8String:buffer] forKey:@"gbBindings"];
    [defaults setInteger:self.slot forKey:@"gbSlot"];
}
/* A short message in the footer that clears itself after a few seconds. */
- (void)setStatus:(NSString *)message {
    self.statusText = message;
    self.statusUntil = SDL_GetTicksNS() + 3000000000ull;
    self.view.needsDisplay = YES;
}
- (NSString *)slotTitle:(NSInteger)slot {
    char path[2048];
    NSString *file = session && nacre_session_state_path(session, (int)slot, path, sizeof(path))
        ? [NSString stringWithUTF8String:path] : nil;
    NSDate *when = file ? [[NSFileManager.defaultManager attributesOfItemAtPath:file error:NULL] fileModificationDate] : nil;
    NSString *stamp = @"empty";
    if (when) {
        NSDateFormatter *formatter = [NSDateFormatter new];
        formatter.dateStyle = NSDateFormatterShortStyle;
        formatter.timeStyle = NSDateFormatterShortStyle;
        stamp = [formatter stringFromDate:when];
    }
    return [NSString stringWithFormat:@"Slot %ld  ·  %@", (long)slot + 1, stamp];
}
- (void)saveState {
    if (![self ready]) return;
    if (![self statesReady]) { [self setStatus:@"Save states need a ROM opened from a file"]; return; }
    char error[256];
    if (nacre_session_save_state(session, (int)self.slot, error, sizeof(error)))
        [self setStatus:[NSString stringWithFormat:@"Saved state to slot %ld", (long)self.slot + 1]];
    else
        [self setStatus:sentence(error)];
}
- (void)loadState {
    if (![self ready]) return;
    if (![self statesReady]) { [self setStatus:@"Save states need a ROM opened from a file"]; return; }
    char error[256];
    if (!nacre_session_load_state(session, (int)self.slot, error, sizeof(error))) {
        [self setStatus:sentence(error)];
        return;
    }
    /* Held keys follow the player's hands, not the snapshot (the pad core does this
     * itself; the keypad is re-sent here). */
    if ([self hasKeypad])
        for (int key = 0; key < 16; key++)
            if (keypadHeld[key]) [self core]->set_keypad([self instance], (unsigned)key, 1);
    [self core]->audio_clear([self instance]);
    self.previous = SDL_GetTicksNS();
    [self updateControls];
    [self setStatus:[NSString stringWithFormat:@"Loaded state from slot %ld", (long)self.slot + 1]];
}
- (void)chooseSlotNumber:(NSInteger)slot {
    if (slot < 0 || slot >= NACRE_STATE_SLOTS) return;
    self.slot = slot;
    [self savePrefs];
    [self setStatus:[NSString stringWithFormat:@"State slot %ld", (long)slot + 1]];
}
- (void)saveStateAction:(id)sender {
    (void)sender; [self saveState]; [self.window makeFirstResponder:self.view];
}
- (void)loadStateAction:(id)sender {
    (void)sender; [self loadState]; [self.window makeFirstResponder:self.view];
}
- (void)chooseSlot:(NSMenuItem *)sender {
    [self chooseSlotNumber:sender.tag]; [self.window makeFirstResponder:self.view];
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(saveStateAction:) || item.action == @selector(loadStateAction:))
        return !self.dialogOpen && [self statesReady];
    if (item.action == @selector(chooseSlot:)) {
        item.state = item.tag == self.slot ? NSControlStateValueOn : NSControlStateValueOff;
        item.title = [self slotTitle:item.tag];
        return !self.dialogOpen && [self statesReady];
    }
    if (item.action == @selector(showControls:)) return !self.dialogOpen && self.controlsSheet == nil;
    if (item.action == @selector(togglePause:)) return [self ready] && ![self faultText];
    if (item.action == @selector(resetROM:)) return [self ready];
    return YES;
}
/* The rebinding sheet. The game is held (dialogOpen) while it is up. */
- (void)showControls:(id)sender {
    (void)sender;
    if (self.dialogOpen || self.controlsSheet) return;
    NSRect frame = NSMakeRect(0, 0, 540, 684);
    NSWindow *sheet = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered defer:NO];
    sheet.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    NacreControlsView *view = [[NacreControlsView alloc] initWithFrame:frame];
    view.controller = self;
    sheet.contentView = view;
    self.controlsSheet = sheet;
    self.dialogOpen = YES; [self releaseKeys]; [self syncAll];
    [self.window beginSheet:sheet completionHandler:^(NSModalResponse response) { (void)response; }];
    [sheet makeFirstResponder:view];
    [self updateControls];
}
- (void)closeControls {
    NSWindow *sheet = self.controlsSheet;
    if (!sheet) return;
    self.controlsSheet = nil; self.dialogOpen = NO; self.previous = SDL_GetTicksNS();
    [self.window endSheet:sheet]; [sheet orderOut:nil];
    [self.window makeFirstResponder:self.view]; [self syncAll]; [self updateControls];
}

/* ---- The screen ---- */
/* Centered in the well: integer-scaled when the core has square pixels so every
 * pixel stays crisp, else fitted to its aspect. Two-level screens are drawn as lit
 * runs in ink; anything else goes through a bitmap scaled with nearest-neighbour. */
- (void)drawFrameInWell:(NSRect)well {
    const NacreVideoFrame *frame = [self frame];
    if (!frame || !frame->pixels || !frame->width || !frame->height) return;
    CGFloat roomW = well.size.width - 48, roomH = well.size.height - 64;
    CGFloat w, h, scale = 1;
    if (frame->aspect <= 0) {
        scale = floor(fmin(roomW / frame->width, roomH / frame->height));
        if (scale < 1) scale = 1;
        w = scale * frame->width;
        h = scale * frame->height;
    } else {
        CGFloat fitW = fmin(roomW, roomH * frame->aspect);
        w = fmax(floor(fitW), 1);
        h = fmax(floor(fitW / frame->aspect), 1);
    }
    CGFloat ox = floor(NSMidX(well) - w / 2), oy = floor(NSMidY(well) - h / 2);
    const uint8_t *pixels = frame->pixels;
    shownRect = NSMakeRect(ox, oy, w, h);
    shownWidth = frame->width;
    shownHeight = frame->height;

    if (frame->format == NACRE_PIXEL_INDEX8 && frame->levels <= 2 && frame->aspect <= 0) {
        [[NSGraphicsContext currentContext] setShouldAntialias:NO];
        [color(INKS[self.palette]) setFill];
        for (unsigned y = 0; y < frame->height; y++) {
            const uint8_t *row = pixels + y * frame->pitch;
            unsigned x = 0;
            while (x < frame->width) {
                if (!row[x]) { x++; continue; }
                unsigned run = x;
                while (run < frame->width && row[run]) run++;
                NSRectFill(NSMakeRect(ox + x * scale, oy + y * scale, (run - x) * scale, scale));
                x = run;
            }
        }
        [[NSGraphicsContext currentContext] setShouldAntialias:YES];
        return;
    }

    if (!frameContext || contextWidth != frame->width || contextHeight != frame->height) {
        if (frameContext) CGContextRelease(frameContext);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        /* 0x00RRGGBB in a native-endian word, the same layout as NACRE_PIXEL_XRGB8888. */
        frameContext = CGBitmapContextCreate(NULL, frame->width, frame->height, 8, 0, space,
            (CGBitmapInfo)((uint32_t)kCGImageAlphaNoneSkipFirst | (uint32_t)kCGBitmapByteOrder32Host));
        CGColorSpaceRelease(space);
        contextWidth = frameContext ? frame->width : 0;
        contextHeight = frameContext ? frame->height : 0;
        if (!frameContext) return;
    }
    uint32_t colors[256];
    if (frame->format == NACRE_PIXEL_INDEX8)
        for (unsigned i = 0; i < 256; i++) colors[i] = 0xFF000000u | shade_rgb(self.palette, i, frame->levels);
    uint8_t *out = CGBitmapContextGetData(frameContext);
    size_t stride = CGBitmapContextGetBytesPerRow(frameContext);
    for (unsigned y = 0; y < frame->height; y++) {
        uint32_t *dest = (uint32_t *)(void *)(out + (size_t)y * stride);
        const uint8_t *row = pixels + y * frame->pitch;
        if (frame->format == NACRE_PIXEL_INDEX8) {
            for (unsigned x = 0; x < frame->width; x++) dest[x] = colors[row[x]];
        } else {
            const uint32_t *rgb = (const uint32_t *)(const void *)row;
            for (unsigned x = 0; x < frame->width; x++) dest[x] = 0xFF000000u | rgb[x];
        }
    }
    CGImageRef image = CGBitmapContextCreateImage(frameContext);
    if (!image) return;
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSaveGState(ctx);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    /* The view is flipped; flip back so the image's first row lands at the top. */
    CGContextTranslateCTM(ctx, ox, oy + h);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
    CGContextRestoreGState(ctx);
    CGImageRelease(image);
}

/* ---- Controls ---- */
- (NSButton *)button:(NSString *)title action:(SEL)action symbol:(NSString *)symbol {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:action];
    b.bezelStyle = NSBezelStyleRounded;
    b.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    b.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil];
    b.imagePosition = NSImageLeading;
    [b setAccessibilityLabel:title];
    [self.view addSubview:b]; return b;
}
- (void)updateControls {
    self.pauseButton.title = self.userPaused ? @"Resume" : @"Pause";
    self.pauseButton.image = [NSImage imageWithSystemSymbolName:self.userPaused ? @"play.fill" : @"pause.fill" accessibilityDescription:nil];
    [self.pauseButton setAccessibilityLabel:self.pauseButton.title];
    self.pauseButton.enabled = [self ready] && ![self faultText];
    self.resetButton.enabled = [self ready];
    self.soundButton.title = self.muted ? @"Sound off" : @"Sound on";
    self.soundButton.image = [NSImage imageWithSystemSymbolName:self.muted ? @"speaker.slash" : @"speaker.wave.2" accessibilityDescription:nil];
    self.soundButton.enabled = [self audioReady];
    [self.soundButton setAccessibilityLabel:self.soundButton.title];
    self.view.needsDisplay = YES;
}
/* The one speed popup shows CPU rates for clock systems and real-time multiples for
 * the rest; each kind keeps its own saved choice. */
- (void)refreshSpeedMenu {
    BOOL clock = [self look]->clockSpeed;
    NSArray<NSString *> *names = clock
        ? @[@"Relaxed · 300 Hz", @"Original · 600 Hz", @"Quick · 1000 Hz"]
        : @[@"Half · 0.5×", @"Normal · 1×", @"Double · 2×"];
    for (NSInteger i = 0; i < 3; i++) [self.speedButton itemAtIndex:i].title = names[(NSUInteger)i];
    [self.speedButton selectItemAtIndex:clock ? self.clockChoice : self.speedChoice];
}
- (void)alert:(NSString *)message {
    BOOL wasOpen = self.dialogOpen; self.dialogOpen = YES;
    [self releaseKeys]; [self syncAll];
    NSAlert *alert = [NSAlert new]; alert.messageText = @"This ROM couldn't run";
    alert.informativeText = message; [alert addButtonWithTitle:@"OK"];
    [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        (void)response; self.dialogOpen = wasOpen; self.previous = SDL_GetTicksNS();
        [self.window makeFirstResponder:self.view]; [self syncAll]; [self updateControls];
    }];
}

/* ---- Opening content ---- */
/* Swaps in a new game. A failed open keeps the one running. */
- (BOOL)openWith:(const NacreCore *)forced path:(NSString *)path data:(NSData *)data
         bundled:(BOOL)bundled url:(NSURL *)url {
    char error[256] = "";
    [self releaseKeys]; /* nothing held carries over to the next game */
    if (!nacre_session_open(session, forced, path ? path.UTF8String : NULL, data.bytes, data.length,
                            error, sizeof(error))) {
        NSString *message = sentence(error);
        if ([self ready]) message = [message stringByAppendingString:@" Your current session has been kept."];
        [self alert:message];
        return NO;
    }
    self.bundled = bundled; self.romURL = bundled ? nil : url;
    self.userPaused = NO; padButtons = 0;
    if (url && !bundled) [NSDocumentController.sharedDocumentController noteNewRecentDocumentURL:url];
    self.previous = SDL_GetTicksNS();
    [self refreshSpeedMenu];
    [self syncAll]; /* opens the audio device at the new core's rate */
    [self.window makeFirstResponder:self.view]; [self updateControls];
    return YES;
}
/* Detects the system from the file's head, then hands the core the whole file, or
 * only its path when the core reads content itself. */
- (void)loadURL:(NSURL *)url {
    NSError *readError = nil;
    NSNumber *size = nil;
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:&readError];
    NSFileHandle *handle = size ? [NSFileHandle fileHandleForReadingFromURL:url error:&readError] : nil;
    NSData *head = handle ? [handle readDataUpToLength:NACRE_PROBE_BYTES error:&readError] : nil;
    [handle closeAndReturnError:NULL];
    if (!head) {
        [self alert:[NSString stringWithFormat:@"%@ Your current session has been kept.",
                         readError ? readError.localizedDescription : @"That file couldn't be read."]];
        return;
    }
    unsigned long long total = size.unsignedLongLongValue;
    if (total == 0 || head.length == 0) {
        [self alert:@"That file is empty. Your current session has been kept."];
        return;
    }
    const char *path = url.path.UTF8String;
    const NacreCore *found = nacre_core_detect(head.bytes, head.length, (size_t)total, path);
    if (!found) {
        [self alert:@"Nacre doesn't recognize that file as a game for any system it runs. "
                    @"Your current session has been kept."];
        return;
    }
    if (found->flags & NACRE_CORE_NEEDS_PATH) {
        [self openWith:found path:url.path data:nil bundled:NO url:url];
        return;
    }
    if (total > MAX_CONTENT) {
        [self alert:@"That file is too large to open. Your current session has been kept."];
        return;
    }
    NSData *content = head.length == total ? head : [NSData dataWithContentsOfURL:url options:0 error:&readError];
    if (!content) {
        [self alert:[NSString stringWithFormat:@"%@ Your current session has been kept.",
                         readError ? readError.localizedDescription : @"That file couldn't be read."]];
        return;
    }
    [self openWith:found path:url.path data:content bundled:NO url:url];
}
/* The welcome program is built in. It opens as bytes with no path, so nothing is
 * ever written beside the app. */
- (void)demo:(id)sender {
    (void)sender;
    NSURL *url = [NSBundle.mainBundle URLForResource:@"nacre" withExtension:@"ch8"];
    NSData *data = url ? [NSData dataWithContentsOfURL:url] : nil;
    if (!data) { [self alert:@"The welcome ROM is missing from this build."]; return; }
    [self openWith:nacre_core_by_id("chip8") path:nil data:data bundled:YES url:nil];
}
- (void)openROM:(id)sender {
    (void)sender; if (self.dialogOpen) return;
    self.dialogOpen = YES; [self releaseKeys]; [self syncAll];
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Open a ROM"; panel.prompt = @"Open ROM";
    panel.allowsMultipleSelection = NO; panel.canChooseDirectories = NO;
    /* Every extension the built-in cores list, then anything at all: detection
     * reads the file, so a misnamed ROM still opens. */
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (const NacreCore *const *c = nacre_cores(); *c; c++) {
        NSString *list = [NSString stringWithUTF8String:(*c)->extensions ? (*c)->extensions : ""];
        for (NSString *extension in [list componentsSeparatedByString:@";"]) {
            UTType *type = extension.length ? [UTType typeWithFilenameExtension:extension] : nil;
            if (type && ![types containsObject:type]) [types addObject:type];
        }
    }
    [types addObject:UTTypeData];
    panel.allowedContentTypes = types;
    panel.allowsOtherFileTypes = YES;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        self.dialogOpen = NO;
        if (response == NSModalResponseOK && panel.URL) [self loadURL:panel.URL];
        self.previous = SDL_GetTicksNS(); [self.window makeFirstResponder:self.view];
        [self syncAll]; [self updateControls];
    }];
}
- (void)togglePause:(id)sender {
    (void)sender; if (![self ready] || [self faultText]) return;
    self.userPaused = !self.userPaused; [self releaseKeys];
    [self syncAll]; [self updateControls]; [self.window makeFirstResponder:self.view];
}
- (void)resetROM:(id)sender {
    (void)sender; if (![self ready]) return;
    /* The battery save is written first and kept across the power cycle. */
    [self releaseKeys];
    nacre_session_restart(session);
    self.userPaused = NO;
    self.previous = SDL_GetTicksNS(); [self syncAll]; [self updateControls];
    [self.window makeFirstResponder:self.view];
}
- (void)toggleSound:(id)sender {
    (void)sender; self.muted = !self.muted;
    if (!snapshotPath) [NSUserDefaults.standardUserDefaults setBool:self.muted forKey:@"muted"];
    [self syncAll]; [self updateControls]; [self.window makeFirstResponder:self.view];
}
- (void)changePalette:(id)sender {
    (void)sender; self.palette = self.paletteButton.indexOfSelectedItem;
    if (!snapshotPath) [NSUserDefaults.standardUserDefaults setInteger:self.palette forKey:@"palette"];
    self.view.needsDisplay = YES; [self.window makeFirstResponder:self.view];
}
- (void)changeSpeed:(id)sender {
    (void)sender;
    NSInteger i = self.speedButton.indexOfSelectedItem;
    if (i < 0 || i > 2) return;
    NSUserDefaults *defaults = snapshotPath ? nil : NSUserDefaults.standardUserDefaults;
    const NacreCore *c = [self core];
    if ([self look]->clockSpeed && c) {
        self.clockChoice = i;
        char hz[16];
        snprintf(hz, sizeof(hz), "%u", CLOCK_HZ[i]);
        nacre_session_set_option(session, c->id, "cpu_hz", hz);
        [defaults setInteger:CLOCK_HZ[i] forKey:@"cpuHz"];
    } else {
        self.speedChoice = i;
        [defaults setInteger:SPEED_PERCENT[i] forKey:@"gbSpeed"];
    }
    [self syncAll];
    [self.window makeFirstResponder:self.view];
}

/* ---- The clock ---- */
- (void)tick:(NSTimer *)timer {
    (void)timer;
    uint64_t now = SDL_GetTicksNS(), elapsed = now - self.previous; self.previous = now;
    [self pollPad];
    ffOn = (ffKey || (padHeld & GBI_BIT(GBI_FAST_FORWARD)) != 0) && ![self paused];
    if (self.statusText && now >= self.statusUntil) self.statusText = nil;
    [self syncSession];
    /* Runs the game and writes the battery save once it has settled. */
    if (session && !nacre_session_tick(session, elapsed, now)) {
        [self releaseKeys];
        [self updateControls];
        [self alert:sentence([self faultText])];
    }
    [self pumpAudio];
    self.view.needsDisplay = YES;
}
- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification; self.focusPaused = YES; [self releaseKeys]; [self syncAll];
    if (session) nacre_session_flush(session);
}
- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification; self.focusPaused = NO; self.previous = SDL_GetTicksNS(); [self syncAll];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender; return YES; }
- (BOOL)application:(NSApplication *)sender openFile:(NSString *)filename {
    (void)sender;
    NSURL *url = [NSURL fileURLWithPath:filename];
    if (!self.window) self.pendingURL = url;
    else [self loadURL:url];
    return YES;
}

/* ---- Menus ---- */
- (void)addMenu:(NSMenu *)menu title:(NSString *)title action:(SEL)action key:(NSString *)key {
    NSMenuItem *item = [menu addItemWithTitle:title action:action keyEquivalent:key]; item.target = self;
}
- (void)buildMenus {
    NSMenu *bar = [NSMenu new]; NSApp.mainMenu = bar;
    NSMenuItem *app = [bar addItemWithTitle:@"Nacre" action:NULL keyEquivalent:@""];
    app.submenu = [[NSMenu alloc] initWithTitle:@"Nacre"];
    [self addMenu:app.submenu title:@"About Nacre" action:@selector(about:) key:@""];
    [app.submenu addItem:NSMenuItem.separatorItem];
    NSMenuItem *hide = [app.submenu addItemWithTitle:@"Hide Nacre" action:@selector(hide:) keyEquivalent:@"h"]; hide.target = NSApp;
    [app.submenu addItem:NSMenuItem.separatorItem];
    NSMenuItem *quit = [app.submenu addItemWithTitle:@"Quit Nacre" action:@selector(terminate:) keyEquivalent:@"q"]; quit.target = NSApp;
    NSMenuItem *file = [bar addItemWithTitle:@"File" action:NULL keyEquivalent:@""];
    file.submenu = [[NSMenu alloc] initWithTitle:@"File"];
    [self addMenu:file.submenu title:@"Open ROM…" action:@selector(openROM:) key:@"o"];
    [self addMenu:file.submenu title:@"Open bundled demo" action:@selector(demo:) key:@""];
    NSMenuItem *recent = [file.submenu addItemWithTitle:@"Open Recent" action:NULL keyEquivalent:@""];
    recent.submenu = [[NSMenu alloc] initWithTitle:@"Open Recent"];
    recent.submenu.delegate = self;
    NSMenuItem *menuSession = [bar addItemWithTitle:@"Session" action:NULL keyEquivalent:@""];
    menuSession.submenu = [[NSMenu alloc] initWithTitle:@"Session"];
    [self addMenu:menuSession.submenu title:@"Pause / Resume" action:@selector(togglePause:) key:@"p"];
    [self addMenu:menuSession.submenu title:@"Restart ROM" action:@selector(resetROM:) key:@"r"];
    [self addMenu:menuSession.submenu title:@"Toggle Sound" action:@selector(toggleSound:) key:@""];
    [menuSession.submenu addItem:NSMenuItem.separatorItem];
    [self addMenu:menuSession.submenu title:@"Save State" action:@selector(saveStateAction:) key:@"s"];
    [self addMenu:menuSession.submenu title:@"Load State" action:@selector(loadStateAction:) key:@"l"];
    NSMenuItem *slots = [menuSession.submenu addItemWithTitle:@"State Slot" action:NULL keyEquivalent:@""];
    slots.submenu = [[NSMenu alloc] initWithTitle:@"State Slot"];
    for (NSInteger i = 0; i < NACRE_STATE_SLOTS; i++) {
        NSMenuItem *item = [slots.submenu addItemWithTitle:[NSString stringWithFormat:@"Slot %ld", (long)i + 1]
                                                    action:@selector(chooseSlot:) keyEquivalent:@""];
        item.target = self; item.tag = i;
    }
    [menuSession.submenu addItem:NSMenuItem.separatorItem];
    [self addMenu:menuSession.submenu title:@"Controls…" action:@selector(showControls:) key:@","];
    NSMenuItem *view = [bar addItemWithTitle:@"View" action:NULL keyEquivalent:@""];
    view.submenu = [[NSMenu alloc] initWithTitle:@"View"];
    [self addMenu:view.submenu title:@"Toggle Full Screen" action:@selector(fullscreen:) key:@"f"];
    view.submenu.itemArray.lastObject.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    NSMenuItem *window = [bar addItemWithTitle:@"Window" action:NULL keyEquivalent:@""];
    window.submenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [window.submenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [window.submenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    NSApp.windowsMenu = window.submenu;
    [file.submenu addItem:NSMenuItem.separatorItem];
    [file.submenu addItemWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];
}
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    for (NSURL *url in NSDocumentController.sharedDocumentController.recentDocumentURLs) {
        NSMenuItem *item = [menu addItemWithTitle:url.lastPathComponent action:@selector(openRecent:) keyEquivalent:@""];
        item.target = self; item.representedObject = url;
    }
    if (!menu.numberOfItems) {
        NSMenuItem *empty = [menu addItemWithTitle:@"No recent ROMs" action:NULL keyEquivalent:@""];
        empty.enabled = NO;
    }
}
- (void)openRecent:(NSMenuItem *)sender { [self loadURL:sender.representedObject]; }
- (void)fullscreen:(id)sender { [self.window toggleFullScreen:sender]; }
- (void)about:(id)sender {
    (void)sender; [NSApp orderFrontStandardAboutPanelWithOptions:@{
        @"ApplicationName":@"Nacre", @"ApplicationVersion":@"0.2.0",
        @"Credits":[[NSAttributedString alloc] initWithString:@"A small machine for CHIP-8 and Game Boy games.\nMade by tdotlab.\n\nFree software under the GNU GPL, version 3 or later (see LICENSE)."] }];
}

/* ---- Launch ---- */
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    SDL_Init(SDL_INIT_AUDIO);
    [self loadPrefs];
    if (!snapshotPath) pad_init();
    NacreSessionIo io = {io_read, io_write, io_now, NULL};
    NacreHost host = {0};
    host.log = host_log;
    /* A snapshot run never writes saves or states. */
    session = nacre_session_create(&io, &host, snapshotPath ? 0 : 1);
    if (session) {
        char hz[16];
        snprintf(hz, sizeof(hz), "%u", CLOCK_HZ[self.clockChoice]);
        nacre_session_set_option(session, "chip8", "cpu_hz", hz);
    }
    audio = audio_out_create();
    appliedRunning = -1; appliedSpeed = 0;

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1120, 720)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Nacre"; self.window.delegate = self;
    self.window.minSize = NSMakeSize(920, 730);
    self.window.backgroundColor = color(0xF5F3EE);
    self.window.titlebarAppearsTransparent = YES;
    self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    [self.window setFrameAutosaveName:@"NacreSessionWindow"];
    self.view = [[NacreView alloc] initWithFrame:NSMakeRect(0, 0, 1120, 720)];
    self.view.controller = self; self.window.contentView = self.view;
    [self.view registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    self.openButton = [self button:@"Open ROM…" action:@selector(openROM:) symbol:@"folder"];
    self.openButton.contentTintColor = color(0x355A43);
    self.openButton.toolTip = @"Open a game for any system Nacre runs (⌘O)";
    self.pauseButton = [self button:@"Pause" action:@selector(togglePause:) symbol:@"pause.fill"];
    self.pauseButton.toolTip = @"Pause or resume (Space)";
    self.resetButton = [self button:@"Restart" action:@selector(resetROM:) symbol:@"arrow.counterclockwise"];
    self.resetButton.toolTip = @"Restart the loaded ROM (⌘R)";
    self.soundButton = [self button:@"Sound on" action:@selector(toggleSound:) symbol:@"speaker.wave.2"];
    self.soundButton.toolTip = @"Toggle sound (M)";
    self.speedButton = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [self.speedButton addItemsWithTitles:@[@"Relaxed · 300 Hz", @"Original · 600 Hz", @"Quick · 1000 Hz"]];
    [self.speedButton selectItemAtIndex:self.clockChoice];
    self.speedButton.target = self; self.speedButton.action = @selector(changeSpeed:);
    [self.speedButton setAccessibilityLabel:@"Execution speed"]; [self.view addSubview:self.speedButton];
    self.paletteButton = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [self.paletteButton addItemsWithTitles:@[@"Fern", @"Amber", @"Pearl"]];
    [self.paletteButton selectItemAtIndex:self.palette];
    self.paletteButton.target = self; self.paletteButton.action = @selector(changePalette:);
    [self.paletteButton setAccessibilityLabel:@"Display palette"]; [self.view addSubview:self.paletteButton];
    self.view.needsLayout = YES;
    NSApp.applicationIconImage = NacreIcon();
    [self buildMenus]; [self.window center]; [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view]; [NSApp activateIgnoringOtherApps:YES];
    if (!self.pendingURL && launchPath) self.pendingURL = [NSURL fileURLWithPath:launchPath];
    if (self.pendingURL) { [self loadURL:self.pendingURL]; self.pendingURL = nil; }
    if (![self ready]) [self demo:nil];
    [self updateControls];
    self.previous = SDL_GetTicksNS();
    self.timer = [NSTimer timerWithTimeInterval:1.0 / 60 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
    if (snapshotPath) [self performSelector:@selector(captureSnapshot) withObject:nil afterDelay:0.5];
}

/* ---- Self-check (--snapshot) ---- */
/* A minimal 32 KB cartridge with a valid header, so the pad path can be checked
 * without shipping a ROM. It waits for vblank, turns the LCD off, fills the top
 * half of tile 1 with the darkest shade, puts that tile in the map's top left
 * corner, turns the LCD back on and loops: the capture shows an 8x4 dark block in
 * the screen's top-left corner, which proves orientation. */
- (NSData *)snapshotCartridge {
    static const uint8_t logo[48] = {
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
        0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
        0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
    };
    static const uint8_t program[] = {
        0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, /* wait for LY == 144 */
        0xAF, 0xE0, 0x40,                   /* LCD off */
        0x21, 0x10, 0x80, 0x3E, 0xFF, 0x06, 0x08, 0x22, 0x05, 0x20, 0xFC, /* tile 1 rows 0-3 */
        0x3E, 0x01, 0xEA, 0x00, 0x98,       /* map (0,0) = tile 1 */
        0x3E, 0x91, 0xE0, 0x40,             /* LCD and background on */
        0x18, 0xFE,                         /* JR -2 */
    };
    NSMutableData *rom = [NSMutableData dataWithLength:0x8000];
    uint8_t *bytes = rom.mutableBytes;
    memcpy(bytes + 0x104, logo, sizeof(logo));
    memcpy(bytes + 0x134, "NACRECHECK", 10);
    bytes[0x100] = 0xC3; /* JP 0x0150 */
    bytes[0x101] = 0x50;
    bytes[0x102] = 0x01;
    memcpy(bytes + 0x150, program, sizeof(program));
    return rom;
}
- (uint32_t)framesNow {
    const NacreVideoFrame *frame = [self frame];
    return frame ? frame->frame_count : 0;
}
/* Runs the game for `seconds` of emulated time through the session, at whatever
 * speed is chosen. Returns NO on a fault. */
- (BOOL)runFor:(double)seconds {
    [self syncSession];
    const uint64_t step = 16666667;
    for (uint64_t done = 0; done < (uint64_t)(seconds * 1e9); done += step)
        if (!nacre_session_tick(session, step, SDL_GetTicksNS())) return NO;
    return YES;
}
- (NSEvent *)keyEvent:(NSEventType)type code:(unsigned short)code characters:(NSString *)characters
                flags:(NSEventModifierFlags)flags {
    return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:flags timestamp:0
        windowNumber:self.window.windowNumber context:nil characters:characters
        charactersIgnoringModifiers:characters isARepeat:NO keyCode:code];
}
/* Presses and releases Right through the view's key path, then checks frames
 * advance. Returns 0, or base..base+2 for the failing step. */
- (int)checkPadKeys:(int)base {
    NSString *right = [NSString stringWithFormat:@"%C", (unichar)NSRightArrowFunctionKey];
    NSEventModifierFlags flags = NSEventModifierFlagFunction | NSEventModifierFlagNumericPad;
    [self.view keyDown:[self keyEvent:NSEventTypeKeyDown code:124 characters:right flags:flags]];
    if (!(padButtons & NACRE_PAD_BIT(NACRE_PAD_RIGHT))) return base;
    [self.view keyUp:[self keyEvent:NSEventTypeKeyUp code:124 characters:right flags:flags]];
    if (padButtons & NACRE_PAD_BIT(NACRE_PAD_RIGHT)) return base + 1;
    uint32_t frames = [self framesNow];
    if (![self runFor:0.2] || [self framesNow] - frames < 10) return base + 2;
    return 0;
}
/* A state round trip through the core table: save, run on, load back, and refuse
 * a damaged copy. With `frames`, the frame counter must change and come back.
 * Returns 0, or base..base+4. */
- (int)checkStates:(int)base frames:(BOOL)frames {
    const NacreCore *c = [self core];
    void *instance = [self instance];
    size_t size = c && c->state_size ? c->state_size(instance) : 0;
    NSMutableData *blob = [NSMutableData dataWithLength:size];
    uint8_t *bytes = blob.mutableBytes;
    if (!size || c->state_save(instance, bytes, size) != size) return base;
    uint32_t saved = [self framesNow];
    if (![self runFor:0.1] || (frames && [self framesNow] == saved)) return base + 1;
    if (!c->state_load(instance, bytes, size)) return base + 2;
    if (frames && [self framesNow] != saved) return base + 3;
    bytes[size / 2] ^= 0xFF;
    bytes[0] ^= 0xFF; /* a damaged header is refused */
    if (c->state_load(instance, bytes, size)) return base + 4;
    return 0;
}
/* Fast-forward as a held key, a state round trip, and the rebinding table.
 * Returns 0, or base..base+8 for the failing step. */
- (int)checkFeatures:(int)base {
    [self.view keyDown:[self keyEvent:NSEventTypeKeyDown code:48 characters:@"\t" flags:0]];
    if (!ffKey) return base;
    [self.view keyUp:[self keyEvent:NSEventTypeKeyUp code:48 characters:@"\t" flags:0]];
    if (ffKey) return base + 1;

    int stage = [self checkStates:base + 2 frames:YES];
    if (stage) return stage;

    GbBindings original = keys;
    if (gbi_assign(&keys, GBI_A, 0, 48) != GBI_FAST_FORWARD || keys.key[GBI_FAST_FORWARD][0] != -1)
        return base + 7;
    char buffer[256];
    GbBindings parsed;
    BOOL roundTrip = gbi_serialize(&keys, buffer, sizeof(buffer)) > 0 && gbi_parse(&parsed, buffer) &&
                     memcmp(&parsed, &keys, sizeof(parsed)) == 0;
    keys = original;
    return roundTrip ? 0 : base + 8;
}
- (BOOL)captureViewTo:(NSString *)path {
    [self.view layoutSubtreeIfNeeded];
    self.view.needsDisplay = YES;
    [self.view displayIfNeeded];
    NSBitmapImageRep *bitmap = [self.view bitmapImageRepForCachingDisplayInRect:self.view.bounds];
    [self.view cacheDisplayInRect:self.view.bounds toBitmapImageRep:bitmap];
    NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [png writeToFile:path atomically:YES];
}
- (void)finishSnapshot:(int)status {
    if (status == 0) status = [self captureViewTo:snapshotPath] ? 0 : 1;
    snapshotStatus = status;
    [NSApp terminate:nil];
}
/* Exercises native actions and the physical key paths before rendering, like the
 * Windows self-check. Stage numbers say which step failed. */
- (void)captureSnapshot {
    BOOL originalMute = self.muted;
    [self togglePause:nil];
    if (!self.userPaused) { [self finishSnapshot:2]; return; }
    [self togglePause:nil];
    [self toggleSound:nil]; [self toggleSound:nil];
    if (self.muted != originalMute) { [self finishSnapshot:3]; return; }
    [self resetROM:nil];
    if (![self ready] || [self faultText]) { [self finishSnapshot:4]; return; }
    self.focusPaused = NO;
    if (![self hasKeypad]) {
        /* Launched with a cartridge: check its keys, run it a while, and capture it. */
        int stage = [self checkPadKeys:12];
        if (!stage) stage = [self checkFeatures:16];
        if (stage) { [self finishSnapshot:stage]; return; }
        [self finishSnapshot:[self runFor:snapshotSeconds] ? 0 : 15];
        return;
    }
    /* The X key sits at physical key code 7 and maps to keypad key 0. */
    [self.view keyDown:[self keyEvent:NSEventTypeKeyDown code:7 characters:@"x" flags:0]];
    if (!keypadHeld[0]) { [self finishSnapshot:5]; return; }
    [self.view keyUp:[self keyEvent:NSEventTypeKeyUp code:7 characters:@"x" flags:0]];
    if (keypadHeld[0]) { [self finishSnapshot:6]; return; }
    int stage = [self checkStates:40 frames:NO];
    if (stage) { [self finishSnapshot:stage]; return; }

    /* The pad path on a stand-in cartridge, picked by detection, then back. */
    NSURL *original = self.romURL;
    BOOL bundled = self.bundled;
    if (![self openWith:NULL path:nil data:[self snapshotCartridge] bundled:NO url:nil] ||
        [self hasKeypad] || ![self hasPad] || [self faultText]) {
        [self finishSnapshot:7]; return;
    }
    /* Let the stand-in finish drawing before counting frames. */
    if (![self runFor:0.1]) { [self finishSnapshot:7]; return; }
    stage = [self checkPadKeys:8];
    if (!stage) stage = [self checkFeatures:30];
    if (stage) { [self finishSnapshot:stage]; return; }
    /* A second capture beside the first shows the pad screen and keycaps. */
    NSString *padShot = [snapshotPath.stringByDeletingPathExtension stringByAppendingString:@"-gb.png"];
    if (![self captureViewTo:padShot]) { [self finishSnapshot:1]; return; }
    if (bundled || !original) [self demo:nil];
    else [self loadURL:original];
    if (![self hasKeypad]) { [self finishSnapshot:11]; return; }

    [self finishSnapshot:[self runFor:snapshotSeconds] ? 0 : 45];
}
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification; [self.timer invalidate];
    /* Writes a pending battery save, then shuts the core down. */
    if (session) nacre_session_destroy(session);
    session = NULL;
    audio_out_destroy(audio);
    audio = NULL;
    if (frameContext) CGContextRelease(frameContext);
    frameContext = NULL;
    pad_shutdown();
    SDL_Quit();
}
@end

int main(int argc, const char **argv) {
    @autoreleasepool {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
                snapshotPath = [NSString stringWithUTF8String:argv[++i]];
            } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
                double seconds = atof(argv[++i]);
                snapshotSeconds = seconds < 0 ? 0 : seconds > 600 ? 600 : seconds;
            } else if (argv[i][0] != '-') {
                NSString *path = [NSString stringWithUTF8String:argv[i]];
                if (path && [NSFileManager.defaultManager fileExistsAtPath:path]) launchPath = path;
            }
        }
        NSApplication *app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        __attribute__((objc_precise_lifetime)) NacreController *controller = [NacreController new];
        app.delegate = controller; [app run];
    }
    return snapshotStatus;
}
