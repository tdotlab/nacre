#import <Cocoa/Cocoa.h>
#import "branding.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include <time.h>
#include "chip8.h"
#include "scheduler.h"
#include "audio.h"
#include "gb.h"
#include "gb_input.h"
#include "../desktop/pad.h"

static NSString *snapshotPath;
static NSString *launchPath;
static double snapshotSeconds = 0.2;
static int snapshotStatus;

static const uint32_t CHIP_HZ[3] = {300, 600, 1000};
static const unsigned GB_PERCENT[3] = {50, 100, 200};
static const unsigned GB_FAST_PERCENT = 400; /* while fast-forward is held */
#define GB_STATE_SLOTS 4
static const NSUInteger MAX_GB_ROM = 8u << 20; /* the largest MBC5 cartridge */
static const unsigned INKS[3] = {0xC3DCC7, 0xEBD8AF, 0xE8E9DF};

/* AppKit owns the UI thread. Only the SDL audio callback runs off-thread. */
typedef struct {
    SDL_AudioStream *stream;
    SDL_AtomicInt active;
    Chip8AudioOscillator oscillator;
} NacreAudio;

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream,
                                  int additional, int total) {
    (void)total;
    NacreAudio *audio = userdata;
    int16_t samples[1024];
    int left = (additional + 1) / 2;
    while (left > 0) {
        int count = left < 1024 ? left : 1024;
        chip8_audio_render_samples(&audio->oscillator, samples, (size_t)count,
                                  SDL_GetAtomicInt(&audio->active));
        if (!SDL_PutAudioStreamData(stream, samples, count * (int)sizeof(int16_t))) return;
        left -= count;
    }
}

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
static int chip_key(unsigned short code) {
    /* Physical ANSI positions, independent of keyboard layout. */
    switch (code) {
        case 18:return 1; case 19:return 2; case 20:return 3; case 21:return 12;
        case 12:return 4; case 13:return 5; case 14:return 6; case 15:return 13;
        case 0:return 7; case 1:return 8; case 2:return 9; case 3:return 14;
        case 6:return 10; case 7:return 0; case 8:return 11; case 9:return 15;
        default:return -1;
    }
}
/* Game Boy keys are bound by physical key code (the same on any layout):
 * arrows and WASD for the D-pad, X/K for A, Z/J for B, Delete or the right Shift
 * for Select, Return or Enter for Start; Tab fast-forwards, F5 and F7 save and
 * load, F6 changes the save slot. */
static const int DEFAULT_KEYS[GBI_COUNT][GBI_KEYS_PER_ACTION] = {
    {124, 2}, {123, 0}, {126, 13}, {125, 1}, /* Right, Left, Up, Down */
    {7, 40}, {6, 38},                        /* A, B */
    {51, 60}, {36, 76},                      /* Select, Start */
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
/* Four shades from the palette's ink down to a deep tone just above the well, so
 * a Game Boy screen reads as the same phosphor as CHIP-8 pixels. */
static unsigned gb_shade_rgb(NSInteger palette, int shade) {
    const unsigned dark = 0x1E2A25;
    unsigned ink = INKS[palette], out = 0;
    for (int channel = 0; channel < 3; channel++) {
        int shift = channel * 8;
        int a = (int)((ink >> shift) & 0xFF), b = (int)((dark >> shift) & 0xFF);
        out |= (unsigned)(a + (b - a) * shade / 3) << shift;
    }
    return out;
}

@class NacreController;
@interface NacreView : NSView
@property(nonatomic, weak) NacreController *controller;
@property(nonatomic) BOOL dropActive;
@end
@interface NacreController : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate> {
@public
    Chip8 machine;
    Chip8Scheduler scheduler;
    NacreAudio audio;
    Gb *gb;
    SDL_AudioStream *gbStream;
    uint8_t gbHeld[GB_BUTTON_COUNT]; /* what the console sees: keyboard or controller */
    uint8_t kbHeld[GB_BUTTON_COUNT]; /* what the keyboard holds */
    unsigned padHeld, padSeen;       /* controller actions now / at the last poll */
    BOOL ffKey, ffOn;                /* fast-forward key held; fast-forward running */
    GbBindings keys;
    CGContextRef gbContext;
}
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NacreView *view;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSURL *romURL;
@property(nonatomic, strong) NSURL *pendingURL;
@property(nonatomic, strong) NSData *romData;
@property(nonatomic, strong) NSString *fault;
@property(nonatomic, strong) NSString *savePath;
@property(nonatomic, strong) NSString *saveNote;
@property(nonatomic) BOOL isGB;
@property(nonatomic) BOOL gbStreamRunning;
@property(nonatomic) NSInteger gbSpeed;
@property(nonatomic) NSInteger chipSpeed;
@property(nonatomic) uint64_t saveDue;
@property(nonatomic) BOOL userPaused;
@property(nonatomic) BOOL focusPaused;
@property(nonatomic) BOOL dialogOpen;
@property(nonatomic) BOOL muted;
@property(nonatomic) BOOL audioAvailable;
@property(nonatomic) NSInteger palette;
@property(nonatomic) uint64_t previous;
@property(nonatomic) NSInteger slot; /* save-state slot, 0..GB_STATE_SLOTS-1 */
@property(nonatomic, strong) NSString *statusText;
@property(nonatomic) uint64_t statusUntil;
@property(nonatomic, strong) NSWindow *controlsSheet;
@property(nonatomic, strong) NSButton *openButton;
@property(nonatomic, strong) NSButton *pauseButton;
@property(nonatomic, strong) NSButton *resetButton;
@property(nonatomic, strong) NSButton *soundButton;
@property(nonatomic, strong) NSPopUpButton *paletteButton;
@property(nonatomic, strong) NSPopUpButton *speedButton;
- (BOOL)paused;
- (void)syncAudio;
- (void)openROM:(id)sender;
- (void)togglePause:(id)sender;
- (void)resetROM:(id)sender;
- (void)toggleSound:(id)sender;
- (void)changePalette:(id)sender;
- (void)changeSpeed:(id)sender;
- (void)loadURL:(NSURL *)url;
- (void)releaseKeys;
- (void)updateControls;
- (void)setGameBoyButton:(int)button pressed:(BOOL)pressed;
- (void)gameBoyAction:(int)action pressed:(BOOL)pressed repeat:(BOOL)repeat;
- (void)savePrefs;
- (void)showControls:(id)sender;
- (void)closeControls;
- (void)drawGameBoyInWell:(NSRect)well;
@end

/* The rebinding sheet: every action with its two keys. */
@interface NacreControlsView : NSView
@property(nonatomic, weak) NacreController *controller;
@property(nonatomic) int action, cellSlot;
@property(nonatomic) BOOL capturing;
@property(nonatomic, strong) NSString *note;
@end

static void timer_tick(Chip8 *chip8, void *userdata) {
    (void)chip8;
    NacreController *controller = (__bridge NacreController *)userdata;
    [controller syncAudio];
}

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
    if (self.controller.isGB) {
        int action = gbi_lookup(&self.controller->keys, event.keyCode);
        if (action >= 0) [self.controller gameBoyAction:action pressed:YES repeat:event.isARepeat];
    } else {
        int key = chip_key(event.keyCode);
        if (key >= 0 && ![self.controller paused] && !event.isARepeat)
            chip8_set_key(&self.controller->machine, (uint8_t)key, 1);
    }
    self.needsDisplay = YES;
}
- (void)keyUp:(NSEvent *)event {
    if (self.controller.isGB) {
        int action = gbi_lookup(&self.controller->keys, event.keyCode);
        if (action >= 0) [self.controller gameBoyAction:action pressed:NO repeat:NO];
    } else {
        int key = chip_key(event.keyCode);
        if (key >= 0) chip8_set_key(&self.controller->machine, (uint8_t)key, 0);
    }
    self.needsDisplay = YES;
}
- (void)flagsChanged:(NSEvent *)event {
    /* Shift is a modifier, so it arrives here rather than in keyDown. The device-dependent
     * bits say which one moved: 0x2 is the left Shift, 0x4 the right. */
    unsigned short code = event.keyCode;
    if (self.controller.isGB && (code == 56 || code == 60)) {
        BOOL down = (event.modifierFlags & (code == 60 ? 0x4 : 0x2)) != 0;
        int action = gbi_lookup(&self.controller->keys, code);
        if (action >= 0) {
            [self.controller gameBoyAction:action pressed:down repeat:NO];
            self.needsDisplay = YES;
        }
    }
    [super flagsChanged:event];
}
- (void)mouseDown:(NSEvent *)event { (void)event; [self.window makeFirstResponder:self]; }
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
    if (c.isGB) {
        /* A D-pad cross with A and B beside it, then Select and Start: the Game Boy's
         * own layout, labelled with the keyboard keys that press them. */
        static const struct { int button; int col, row, cols; CGFloat size; } caps[] = {
            {GB_BUTTON_UP, 1, 0, 1, 15},    {GB_BUTTON_LEFT, 0, 1, 1, 15},
            {GB_BUTTON_RIGHT, 2, 1, 1, 15}, {GB_BUTTON_DOWN, 1, 2, 1, 15},
            {GB_BUTTON_A, 3, 0, 1, 12},     {GB_BUTTON_B, 3, 1, 1, 12},
            {GB_BUTTON_SELECT, 0, 3, 2, 9}, {GB_BUTTON_START, 2, 3, 2, 9},
        };
        for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
            NSString *label = short_key(first_key(&c->keys, caps[i].button));
            if (caps[i].button == GB_BUTTON_SELECT) label = [label stringByAppendingString:@" SELECT"];
            if (caps[i].button == GB_BUTTON_START) label = [label stringByAppendingString:@" START"];
            keycap(26 + caps[i].col * 44, 444 + caps[i].row * 39, caps[i].cols * 44 - 8, label,
                   c->gbHeld[caps[i].button] != 0, caps[i].size);
        }
        int fast = first_key(&c->keys, GBI_FAST_FORWARD);
        text(fast >= 0 ? [NSString stringWithFormat:@"Hold %@ to fast-forward", key_name(fast)]
                       : @"Fast-forward is unbound",
             NSMakeRect(26, 600, 190, 18), 10, NSFontWeightRegular, 0x7F847A, NO);
        text([NSString stringWithFormat:@"%@ saves  ·  %@ loads  ·  slot %ld",
                 short_key(first_key(&c->keys, GBI_SAVE_STATE)),
                 short_key(first_key(&c->keys, GBI_LOAD_STATE)), (long)c.slot + 1],
             NSMakeRect(26, 616, 190, 18), 10, NSFontWeightRegular, 0x7F847A, NO);
        text(@"Space to pause  ·  M to mute", NSMakeRect(26, 632, 190, 18), 10,
             NSFontWeightRegular, 0x7F847A, NO);
        text(@"Session ▸ Controls… to rebind", NSMakeRect(26, 648, 190, 18), 10,
             NSFontWeightRegular, 0x7F847A, NO);
        text(@"GAME BOY  /  160 × 144", NSMakeRect(26, h - 36, 180, 18), 10,
             NSFontWeightMedium, 0x878C80, YES);
    } else {
        const char *host = "1234QWERASDFZXCV";
        const int keys[16] = {1,2,3,12,4,5,6,13,7,8,9,14,10,0,11,15};
        for (int i = 0; i < 16; i++)
            keycap(26 + (i % 4) * 44, 444 + (i / 4) * 39, 36,
                   [NSString stringWithFormat:@"%c", host[i]], c->machine.keys[keys[i]] != 0, 12);
        text(@"Space to pause  ·  M to mute", NSMakeRect(26, 610, 180, 18), 10,
             NSFontWeightRegular, 0x7F847A, NO);
        text(@"CHIP-8  /  64 × 32", NSMakeRect(26, h - 36, 180, 18), 10,
             NSFontWeightMedium, 0x878C80, YES);
    }

    BOOL bundled = [c.romURL.path hasPrefix:NSBundle.mainBundle.resourcePath];
    NSString *name = c.romURL ? c.romURL.lastPathComponent.stringByDeletingPathExtension : @"Your next little world";
    if (bundled) name = @"Welcome to Nacre";
    text(name, NSMakeRect(254, 27, w - 430, 32), 23, NSFontWeightSemibold, 0x293D34, NO);
    NSString *kind = bundled ? @"PRESS A KEYPAD KEY TO SAY HELLO" : @"CHIP-8 ROM";
    if (c.isGB) {
        NSString *cart = [NSString stringWithUTF8String:gb_title(c->gb)];
        kind = cart.length ? [@"GAME BOY  ·  " stringByAppendingString:cart] : @"GAME BOY ROM";
    }
    text(kind, NSMakeRect(255, 63, w - 400, 18), 10, NSFontWeightMedium, 0x7B8479, YES);
    BOOL paused = [c paused];
    BOOL fast = c->ffOn && !paused;
    fill(NSMakeRect(w - 133, 42, 6, 6), c.fault ? 0xB66C52 : paused ? 0xB09361 : 0x608C70, 3);
    text(c.fault ? @"STOPPED" : paused ? @"PAUSED" : fast ? @"FAST 4×" : @"RUNNING",
         NSMakeRect(w - 118, 37, 90, 20), 10, NSFontWeightMedium, 0x6C786C, YES);

    NSRect well = NSMakeRect(254, 111, w - 286, h - 233);
    fill(NSOffsetRect(well, 0, 3), 0xD7D8CE, 14);
    fill(well, self.dropActive ? 0x294C3D : 0x18211E, 14);
    if (c.isGB) {
        [c drawGameBoyInWell:well];
    } else {
        CGFloat scale = floor(fmin((well.size.width - 48) / 64, (well.size.height - 64) / 32));
        CGFloat ox = floor(NSMidX(well) - 32 * scale), oy = floor(NSMidY(well) - 16 * scale);
        [[NSGraphicsContext currentContext] setShouldAntialias:NO];
        [color(INKS[c.palette]) setFill];
        for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x++)
            if (c->machine.display[y * 64 + x]) NSRectFill(NSMakeRect(ox + x * scale, oy + y * scale, scale, scale));
        [[NSGraphicsContext currentContext] setShouldAntialias:YES];
    }
    text(@"NACRE", NSMakeRect(well.origin.x + 18, NSMaxY(well) - 25, 100, 14), 8,
         NSFontWeightMedium, 0x64776B, YES);
    if (paused || self.dropActive || c.fault || fast) {
        NSString *badge = self.dropActive ? @"Drop to open ROM" : c.fault ? @"Execution stopped"
                        : paused ? @"Paused" : @"Fast-forward 4×";
        NSRect badgeRect = NSMakeRect(NSMidX(well) - 92, well.origin.y + 18, 184, 30);
        fill(badgeRect, 0x35483C, 15);
        NSMutableParagraphStyle *center = [NSMutableParagraphStyle new]; center.alignment = NSTextAlignmentCenter;
        [badge drawInRect:NSInsetRect(badgeRect, 0, 6) withAttributes:@{
            NSFontAttributeName:[NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName:color(0xDCE8DA), NSParagraphStyleAttributeName:center}];
    }
    NSString *note = c.fault;
    if (!note && (c.isGB ? c->gbStream == NULL : !c.audioAvailable)) note = @"Audio unavailable";
    if (!note && c.isGB) note = c.saveNote;
    if (c.isGB && !c.fault && c.statusText && SDL_GetTicksNS() < c.statusUntil) note = c.statusText;
    if (note) text(note, NSMakeRect(254, h - 34, w - 450, 18), 10, NSFontWeightRegular, 0x7A8276, NO);
    text(c.isGB ? @"59.7 HZ FRAMES" : @"60 HZ TIMERS", NSMakeRect(w - 146, h - 34, 120, 18), 9,
         NSFontWeightMedium, 0x87907F, YES);
}
@end

@implementation NacreControlsView {
    NSButton *restoreButton, *doneButton;
}
static NSRect cell_rect(int row, int slot) {
    return NSMakeRect(slot ? 360 : 200, 104 + row * 31, 140, 25);
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    restoreButton = [NSButton buttonWithTitle:@"Restore defaults" target:self action:@selector(restore:)];
    restoreButton.frame = NSMakeRect(24, 558, 150, 30);
    doneButton = [NSButton buttonWithTitle:@"Done" target:self action:@selector(done:)];
    doneButton.frame = NSMakeRect(frame.size.width - 124, 558, 100, 30);
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
    text(@"Game Boy controls", NSMakeRect(28, 22, 480, 24), 17, NSFontWeightSemibold, 0x293D34, NO);
    text(@"Click a key, then press the new one. Esc cancels; Delete clears.",
         NSMakeRect(28, 50, 490, 18), 11, NSFontWeightRegular, 0x747B72, NO);
    text(@"KEY 1", NSMakeRect(200, 82, 140, 14), 9, NSFontWeightSemibold, 0x83867D, YES);
    text(@"KEY 2", NSMakeRect(360, 82, 140, 14), 9, NSFontWeightSemibold, 0x83867D, YES);
    for (int row = 0; row < GBI_COUNT; row++) {
        CGFloat y = 104 + row * 31;
        if (row == GBI_BUTTONS) fill(NSMakeRect(28, y - 4, 472, 1), 0xD8D8CF, 0);
        text([NSString stringWithUTF8String:gbi_name((GbInput)row)], NSMakeRect(28, y + 4, 160, 18), 12,
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
            text_aligned(label, NSInsetRect(rect, 6, 5), 12, NSFontWeightMedium,
                         here && self.capturing ? 0x355A43 : 0x3A463E, NO, NSTextAlignmentCenter);
        }
    }
    if (self.note) text(self.note, NSMakeRect(28, 478, 480, 16), 11, NSFontWeightMedium, 0xB66C52, NO);
    text(@"Controller: D-pad or left stick, face buttons for A and B, Start and Back.",
         NSMakeRect(28, 500, 490, 16), 10, NSFontWeightRegular, 0x7F847A, NO);
    text(@"Right shoulder or either trigger: fast-forward (hold).",
         NSMakeRect(28, 516, 490, 16), 10, NSFontWeightRegular, 0x7F847A, NO);
    text(@"Left shoulder held + Start, Back or the top button: save, load, next slot.",
         NSMakeRect(28, 532, 490, 16), 10, NSFontWeightRegular, 0x7F847A, NO);
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
- (BOOL)paused { return self.userPaused || self.focusPaused || self.dialogOpen || self.fault != nil; }
- (void)syncAudio {
    SDL_SetAtomicInt(&audio.active, !self.isGB && self.audioAvailable && !self.muted &&
                                    ![self paused] && machine.sound_timer > 0);
    BOOL run = self.isGB && gbStream && !self.muted && ![self paused];
    if (gbStream && run != self.gbStreamRunning) {
        if (run) {
            SDL_ClearAudioStream(gbStream);
            gb_audio_clear(gb);
            SDL_ResumeAudioStreamDevice(gbStream);
        } else {
            SDL_PauseAudioStreamDevice(gbStream);
            SDL_ClearAudioStream(gbStream);
        }
        self.gbStreamRunning = run;
    }
}
/* Moves the samples the Game Boy produced this tick to the device. The emulator
 * runs on the wall clock and the device on its own crystal, so the queue is kept
 * between about 40 and 150 ms: top it up with silence after an underrun and drop
 * a batch when it runs long. Fast and slow speeds resample to keep up. */
- (void)pumpGameBoyAudio {
    static int16_t samples[4096 * 2];
    static const int16_t silence[480 * 2] = {0};
    if (!gb) return;
    if (!self.gbStreamRunning) { gb_audio_clear(gb); return; }
    if (ffOn) { /* four times faster is a squeal, not music: run silent and let the queue drain */
        while (gb_audio_read(gb, samples, 4096) > 0) {}
        return;
    }
    float ratio = GB_PERCENT[self.gbSpeed] / 100.0f;
    SDL_SetAudioStreamFrequencyRatio(gbStream, ratio);
    const int bytesPerMs = (int)(GB_AUDIO_SAMPLE_RATE / 1000u) * 4;
    int queued = SDL_GetAudioStreamQueued(gbStream);
    BOOL drop = queued > (int)(150 * bytesPerMs * ratio);
    if (queued < (int)(20 * bytesPerMs * ratio)) {
        int pad = (int)(40 * bytesPerMs * ratio) - queued;
        while (pad > 0) {
            int chunk = pad < (int)sizeof(silence) ? pad & ~3 : (int)sizeof(silence);
            if (chunk <= 0) break;
            SDL_PutAudioStreamData(gbStream, silence, chunk);
            pad -= chunk;
        }
    }
    size_t frames;
    while ((frames = gb_audio_read(gb, samples, 4096)) > 0)
        if (!drop) SDL_PutAudioStreamData(gbStream, samples, (int)(frames * 4));
}
/* The console sees a button held when the keyboard or a controller holds it. */
- (void)applyButtons {
    for (int button = 0; button < GB_BUTTON_COUNT; button++) {
        uint8_t held = (kbHeld[button] || (padHeld & PAD_BIT(button))) ? 1 : 0;
        gbHeld[button] = held;
        if (gb) gb_set_button(gb, (GbButton)button, held);
    }
}
- (void)setGameBoyButton:(int)button pressed:(BOOL)pressed {
    if (button < 0 || button >= GB_BUTTON_COUNT) return;
    kbHeld[button] = pressed ? 1 : 0;
    [self applyButtons];
}
- (void)releaseKeys {
    for (uint8_t key = 0; key < 16; key++) chip8_set_key(&machine, key, 0);
    memset(kbHeld, 0, sizeof(kbHeld));
    ffKey = NO; padHeld = 0; padSeen = 0;
    [self applyButtons];
}
/* A bound Game Boy key went down or up. */
- (void)gameBoyAction:(int)action pressed:(BOOL)pressed repeat:(BOOL)repeat {
    if (action < GBI_BUTTONS) {
        if (!pressed) [self setGameBoyButton:action pressed:NO];
        else if (![self paused] && !repeat) [self setGameBoyButton:action pressed:YES];
    } else if (action == GBI_FAST_FORWARD) {
        ffKey = pressed;
    } else if (pressed && !repeat) {
        if (action == GBI_SAVE_STATE) [self saveGameBoyState];
        else if (action == GBI_LOAD_STATE) [self loadGameBoyState];
        else if (action == GBI_NEXT_SLOT) [self chooseSlotNumber:(self.slot + 1) % GB_STATE_SLOTS];
    }
}
/* Reads the controllers: held buttons feed the console, and the state hotkeys fire once
 * per press. Only a focused, running window listens, so a game in the background isn't played. */
- (void)pollPad {
    unsigned held = pad_poll();
    const char *note = pad_take_message();
    if (note && self.isGB) [self setStatus:[NSString stringWithUTF8String:note]];
    if (!self.isGB || [self paused]) held = 0;
    unsigned fresh = held & ~padSeen;
    padSeen = held; padHeld = held;
    [self applyButtons];
    if (fresh & PAD_BIT(GBI_SAVE_STATE)) [self saveGameBoyState];
    if (fresh & PAD_BIT(GBI_LOAD_STATE)) [self loadGameBoyState];
    if (fresh & PAD_BIT(GBI_NEXT_SLOT)) [self chooseSlotNumber:(self.slot + 1) % GB_STATE_SLOTS];
}

/* ---- Preferences and Game Boy save states ------------------------------------------------- */
- (void)loadPrefs {
    gbi_set_defaults(&keys, DEFAULT_KEYS);
    self.slot = 0;
    if (snapshotPath) return; /* the self-check always runs on the defaults */
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSString *saved = [defaults stringForKey:@"gbBindings"];
    if (saved) gbi_parse(&keys, saved.UTF8String); /* junk leaves the defaults in place */
    NSInteger slot = [defaults integerForKey:@"gbSlot"];
    if (slot >= 0 && slot < GB_STATE_SLOTS) self.slot = slot;
}
- (void)savePrefs {
    if (snapshotPath) return;
    char text[512];
    if (gbi_serialize(&keys, text, sizeof(text)) == 0) return;
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    [defaults setObject:[NSString stringWithUTF8String:text] forKey:@"gbBindings"];
    [defaults setInteger:self.slot forKey:@"gbSlot"];
}
/* A short message in the footer that clears itself after a few seconds. */
- (void)setStatus:(NSString *)message {
    self.statusText = message;
    self.statusUntil = SDL_GetTicksNS() + 3000000000ull;
    self.view.needsDisplay = YES;
}
/* "Tetris.gb" keeps its states in "Tetris.state1" .. "Tetris.state4". The built-in demo
 * and anything opened without a path have nowhere to put them. */
- (NSString *)statePath:(NSInteger)slot {
    if (!self.romURL || snapshotPath) return nil;
    if ([self.romURL.path hasPrefix:NSBundle.mainBundle.resourcePath]) return nil;
    NSString *extension = [NSString stringWithFormat:@"state%ld", (long)slot + 1];
    return [self.romURL.path.stringByDeletingPathExtension stringByAppendingPathExtension:extension];
}
- (NSString *)slotTitle:(NSInteger)slot {
    NSString *path = [self statePath:slot];
    NSDate *when = path ? [[NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL] fileModificationDate] : nil;
    NSString *stamp = @"empty";
    if (when) {
        NSDateFormatter *formatter = [NSDateFormatter new];
        formatter.dateStyle = NSDateFormatterShortStyle;
        formatter.timeStyle = NSDateFormatterShortStyle;
        stamp = [formatter stringFromDate:when];
    }
    return [NSString stringWithFormat:@"Slot %ld  ·  %@", (long)slot + 1, stamp];
}
- (void)saveGameBoyState {
    if (!self.isGB || !gb || !self.romData) return;
    NSString *path = [self statePath:self.slot];
    if (!path) { [self setStatus:@"Save states need a ROM opened from a file"]; return; }
    size_t capacity = gb_state_size(gb);
    NSMutableData *blob = [NSMutableData dataWithLength:capacity];
    size_t size = capacity ? gb_state_save(gb, blob.mutableBytes, capacity) : 0;
    blob.length = size;
    /* Atomic: written to a temporary file and renamed, so a crash never tears it. */
    if (size > 0 && [blob writeToFile:path options:NSDataWritingAtomic error:NULL])
        [self setStatus:[NSString stringWithFormat:@"Saved state to slot %ld", (long)self.slot + 1]];
    else
        [self setStatus:[NSString stringWithFormat:@"Couldn't save to slot %ld", (long)self.slot + 1]];
}
- (void)loadGameBoyState {
    if (!self.isGB || !gb || !self.romData) return;
    NSString *path = [self statePath:self.slot];
    if (!path) { [self setStatus:@"Save states need a ROM opened from a file"]; return; }
    NSString *slot = [NSString stringWithFormat:@"%ld", (long)self.slot + 1];
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL];
    if (!attributes) { [self setStatus:[@"Slot " stringByAppendingFormat:@"%@ is empty", slot]]; return; }
    NSData *blob = attributes.fileSize <= (1u << 24) ? [NSData dataWithContentsOfFile:path options:0 error:NULL] : nil;
    if (blob && gb_state_load(gb, blob.bytes, blob.length)) {
        /* The state remembers the buttons held when it was saved; what is held now wins. */
        [self applyButtons];
        gb_audio_clear(gb);
        self.previous = SDL_GetTicksNS();
        self.fault = nil;
        [self setStatus:[@"Loaded state from slot " stringByAppendingString:slot]];
        [self updateControls];
    } else {
        [self setStatus:[@"Slot " stringByAppendingFormat:@"%@ doesn't fit this game", slot]];
    }
}
- (void)chooseSlotNumber:(NSInteger)slot {
    if (slot < 0 || slot >= GB_STATE_SLOTS) return;
    self.slot = slot;
    [self savePrefs];
    [self setStatus:[NSString stringWithFormat:@"State slot %ld", (long)slot + 1]];
}
- (void)saveStateAction:(id)sender {
    (void)sender; [self saveGameBoyState]; [self.window makeFirstResponder:self.view];
}
- (void)loadStateAction:(id)sender {
    (void)sender; [self loadGameBoyState]; [self.window makeFirstResponder:self.view];
}
- (void)chooseSlot:(NSMenuItem *)sender {
    [self chooseSlotNumber:sender.tag]; [self.window makeFirstResponder:self.view];
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(saveStateAction:) || item.action == @selector(loadStateAction:))
        return self.isGB && !self.dialogOpen && [self statePath:self.slot] != nil;
    if (item.action == @selector(chooseSlot:)) {
        item.state = item.tag == self.slot ? NSControlStateValueOn : NSControlStateValueOff;
        item.title = [self slotTitle:item.tag];
        return self.isGB && !self.dialogOpen;
    }
    if (item.action == @selector(showControls:)) return !self.dialogOpen && self.controlsSheet == nil;
    return YES;
}
/* The rebinding sheet. The game is held (dialogOpen) while it is up. */
- (void)showControls:(id)sender {
    (void)sender;
    if (self.dialogOpen || self.controlsSheet) return;
    NSRect frame = NSMakeRect(0, 0, 540, 600);
    NSWindow *sheet = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered defer:NO];
    sheet.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    NacreControlsView *view = [[NacreControlsView alloc] initWithFrame:frame];
    view.controller = self;
    sheet.contentView = view;
    self.controlsSheet = sheet;
    self.dialogOpen = YES; [self releaseKeys]; [self syncAudio];
    [self.window beginSheet:sheet completionHandler:^(NSModalResponse response) { (void)response; }];
    [sheet makeFirstResponder:view];
    [self updateControls];
}
- (void)closeControls {
    NSWindow *sheet = self.controlsSheet;
    if (!sheet) return;
    self.controlsSheet = nil; self.dialogOpen = NO; self.previous = SDL_GetTicksNS();
    [self.window endSheet:sheet]; [sheet orderOut:nil];
    [self.window makeFirstResponder:self.view]; [self syncAudio]; [self updateControls];
}
- (void)drawGameBoyInWell:(NSRect)well {
    if (!gb) return;
    CGFloat scale = floor(fmin((well.size.width - 48) / GB_SCREEN_WIDTH,
                              (well.size.height - 64) / GB_SCREEN_HEIGHT));
    if (scale < 1) scale = 1;
    CGFloat w = GB_SCREEN_WIDTH * scale, h = GB_SCREEN_HEIGHT * scale;
    CGFloat ox = floor(NSMidX(well) - w / 2), oy = floor(NSMidY(well) - h / 2);
    if (!gbContext) {
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        gbContext = CGBitmapContextCreate(NULL, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT, 8,
            GB_SCREEN_WIDTH * 4, space, (CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(space);
        if (!gbContext) return;
    }
    uint8_t rgb[4][3];
    for (int i = 0; i < 4; i++) {
        unsigned shade = gb_shade_rgb(self.palette, i);
        rgb[i][0] = (uint8_t)(shade >> 16); rgb[i][1] = (uint8_t)(shade >> 8); rgb[i][2] = (uint8_t)shade;
    }
    uint8_t *out = CGBitmapContextGetData(gbContext);
    size_t stride = CGBitmapContextGetBytesPerRow(gbContext);
    const uint8_t *shades = gb_framebuffer(gb);
    for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
        uint8_t *row = out + (size_t)y * stride;
        for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
            const uint8_t *c = rgb[shades[y * GB_SCREEN_WIDTH + x] & 3];
            row[x * 4] = c[0]; row[x * 4 + 1] = c[1]; row[x * 4 + 2] = c[2]; row[x * 4 + 3] = 255;
        }
    }
    CGImageRef image = CGBitmapContextCreateImage(gbContext);
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
    self.pauseButton.enabled = self.romData != nil && !self.fault;
    self.resetButton.enabled = self.romData != nil;
    self.soundButton.title = self.muted ? @"Sound off" : @"Sound on";
    self.soundButton.image = [NSImage imageWithSystemSymbolName:self.muted ? @"speaker.slash" : @"speaker.wave.2" accessibilityDescription:nil];
    self.soundButton.enabled = self.isGB ? gbStream != NULL : self.audioAvailable;
    [self.soundButton setAccessibilityLabel:self.soundButton.title];
    self.view.needsDisplay = YES;
}
/* The one speed popup shows CPU rates for CHIP-8 and real-time multiples for the
 * Game Boy; each keeps its own saved choice. */
- (void)refreshSpeedMenu {
    NSArray<NSString *> *names = self.isGB
        ? @[@"Half · 0.5×", @"Normal · 1×", @"Double · 2×"]
        : @[@"Relaxed · 300 Hz", @"Original · 600 Hz", @"Quick · 1000 Hz"];
    for (NSInteger i = 0; i < 3; i++) [self.speedButton itemAtIndex:i].title = names[(NSUInteger)i];
    [self.speedButton selectItemAtIndex:self.isGB ? self.gbSpeed : self.chipSpeed];
}
- (void)alert:(NSString *)message {
    BOOL wasOpen = self.dialogOpen; self.dialogOpen = YES;
    [self releaseKeys]; [self syncAudio];
    NSAlert *alert = [NSAlert new]; alert.messageText = @"This ROM couldn't run";
    alert.informativeText = message; [alert addButtonWithTitle:@"OK"];
    [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        (void)response; self.dialogOpen = wasOpen; self.previous = SDL_GetTicksNS();
        [self.window makeFirstResponder:self.view]; [self syncAudio]; [self updateControls];
    }];
}

/* ---- Game Boy battery saves: "Tetris.gb" saves to "Tetris.sav" beside it. ---- */
- (void)writeSave {
    self.saveDue = 0;
    if (!gb || !self.isGB || !self.savePath || snapshotPath || !gb_save_dirty(gb)) return;
    gb_set_wall_clock(gb, (int64_t)time(NULL));
    size_t capacity = gb_save_size(gb);
    NSMutableData *data = [NSMutableData dataWithLength:capacity];
    size_t size = capacity ? gb_save_write(gb, data.mutableBytes, capacity) : 0;
    data.length = size;
    NSString *name = self.savePath.lastPathComponent;
    /* Atomic: written to a temporary file and renamed, so a crash never tears it. */
    if (size > 0 && [data writeToFile:self.savePath options:NSDataWritingAtomic error:NULL]) {
        gb_save_mark_clean(gb);
        self.saveNote = [@"Saved to " stringByAppendingString:name];
    } else {
        self.saveNote = [@"Couldn't write " stringByAppendingString:name];
    }
    self.view.needsDisplay = YES;
}
- (void)readSave {
    self.saveNote = nil;
    if (!self.savePath) return;
    gb_set_wall_clock(gb, (int64_t)time(NULL));
    NSString *name = self.savePath.lastPathComponent;
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:self.savePath error:NULL];
    if (!attributes) {
        self.saveNote = [@"Saves to " stringByAppendingString:name];
        return;
    }
    NSData *data = attributes.fileSize <= (1u << 20)
        ? [NSData dataWithContentsOfFile:self.savePath options:0 error:NULL] : nil;
    if (data && gb_save_load(gb, data.bytes, data.length)) {
        self.saveNote = [@"Loaded " stringByAppendingString:name];
    } else {
        /* Leave a save we can't use alone rather than overwrite it. */
        self.saveNote = [NSString stringWithFormat:@"Ignored %@; it doesn't fit this cartridge", name];
        self.savePath = nil;
    }
}

/* A file with a Game Boy cartridge header runs on the Game Boy core; anything else
 * must be a CHIP-8 program. A failed load keeps the current session. */
- (BOOL)loadData:(NSData *)data url:(NSURL *)url {
    BOOL bundled = url && [url.path hasPrefix:NSBundle.mainBundle.resourcePath];
    BOOL gbRom = !bundled && gb_rom_looks_valid(data.bytes, data.length);
    char error[256];
    Chip8 candidate;
    chip8_init(&candidate, 0x4E414352u);
    if (gbRom) {
        if (!gb) { [self alert:@"The Game Boy core couldn't start, so this ROM can't run."]; return NO; }
        [self writeSave]; /* the cartridge being replaced */
        if (!gb_load_rom(gb, data.bytes, data.length, error, sizeof(error))) {
            [self alert:[NSString stringWithFormat:@"%s Your current session has been kept.", error]];
            return NO;
        }
    } else {
        if (data.length == 0) {
            [self alert:@"That file is empty. Your current session has been kept."]; return NO;
        }
        if (data.length > CHIP8_MEMORY_SIZE - CHIP8_PROGRAM_START) {
            [self alert:@"That file has no Game Boy cartridge header, and a CHIP-8 ROM can be at "
                        @"most 3,584 bytes. Your current session has been kept."];
            return NO;
        }
        if (!chip8_load_bytes(&candidate, data.bytes, data.length, error, sizeof(error))) {
            [self alert:[NSString stringWithUTF8String:error]]; return NO;
        }
        [self writeSave];
    }
    [self releaseKeys];
    self.isGB = gbRom; self.romURL = url; self.romData = data; self.fault = nil;
    self.savePath = nil; self.saveNote = nil; self.saveDue = 0;
    if (gbRom) {
        if (gb_has_battery(gb) && url) {
            self.savePath = [url.path.stringByDeletingPathExtension stringByAppendingPathExtension:@"sav"];
            [self readSave];
        }
    } else {
        machine = candidate;
        chip8_scheduler_init(&scheduler, CHIP_HZ[self.chipSpeed]);
    }
    [self refreshSpeedMenu];
    self.userPaused = NO; self.previous = SDL_GetTicksNS();
    if (url && !bundled) [NSDocumentController.sharedDocumentController noteNewRecentDocumentURL:url];
    [self.window makeFirstResponder:self.view]; [self syncAudio]; [self updateControls];
    return YES;
}
- (void)loadURL:(NSURL *)url {
    NSError *readError = nil;
    NSNumber *size = nil;
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:&readError];
    if (!size) {
        [self alert:readError ? readError.localizedDescription : @"That file couldn't be read."]; return;
    }
    if (size.unsignedLongLongValue > MAX_GB_ROM) {
        [self alert:@"That file is larger than any Game Boy cartridge (8 MB). Your current session has been kept."];
        return;
    }
    NSData *data = [NSData dataWithContentsOfURL:url options:0 error:&readError];
    if (!data) { [self alert:readError.localizedDescription]; return; }
    [self loadData:data url:url];
}
- (void)openROM:(id)sender {
    (void)sender; if (self.dialogOpen) return;
    self.dialogOpen = YES; [self releaseKeys]; [self syncAudio];
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Open a ROM"; panel.prompt = @"Open ROM";
    panel.allowsMultipleSelection = NO; panel.canChooseDirectories = NO;
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *extension in @[@"ch8", @"rom", @"gb", @"gbc"]) {
        UTType *type = [UTType typeWithFilenameExtension:extension];
        if (type) [types addObject:type];
    }
    [types addObject:UTTypeData];
    panel.allowedContentTypes = types;
    panel.allowsOtherFileTypes = YES;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        self.dialogOpen = NO;
        if (response == NSModalResponseOK && panel.URL) [self loadURL:panel.URL];
        self.previous = SDL_GetTicksNS(); [self.window makeFirstResponder:self.view];
        [self syncAudio]; [self updateControls];
    }];
}
- (void)togglePause:(id)sender {
    (void)sender; if (!self.romData || self.fault) return;
    self.userPaused = !self.userPaused; [self releaseKeys];
    [self syncAudio]; [self updateControls]; [self.window makeFirstResponder:self.view];
}
- (void)resetROM:(id)sender {
    (void)sender; if (!self.romData) return;
    if (self.isGB) {
        /* gb_reset keeps the cartridge RAM, so the battery save carries over. */
        [self writeSave];
        gb_reset(gb); gb_audio_clear(gb); [self releaseKeys];
    } else {
        Chip8 candidate; chip8_init(&candidate, 0x4E414352u); char error[256];
        if (!chip8_load_bytes(&candidate, self.romData.bytes, self.romData.length, error, sizeof(error))) return;
        machine = candidate;
        chip8_scheduler_init(&scheduler, CHIP_HZ[self.chipSpeed]);
    }
    self.fault = nil; self.userPaused = NO;
    self.previous = SDL_GetTicksNS(); [self syncAudio]; [self updateControls];
    [self.window makeFirstResponder:self.view];
}
- (void)toggleSound:(id)sender {
    (void)sender; self.muted = !self.muted;
    [NSUserDefaults.standardUserDefaults setBool:self.muted forKey:@"muted"];
    [self syncAudio]; [self updateControls]; [self.window makeFirstResponder:self.view];
}
- (void)changePalette:(id)sender {
    (void)sender; self.palette = self.paletteButton.indexOfSelectedItem;
    [NSUserDefaults.standardUserDefaults setInteger:self.palette forKey:@"palette"];
    self.view.needsDisplay = YES; [self.window makeFirstResponder:self.view];
}
- (void)changeSpeed:(id)sender {
    (void)sender;
    NSInteger i = self.speedButton.indexOfSelectedItem;
    if (i < 0 || i > 2) return;
    if (self.isGB) {
        self.gbSpeed = i;
        [NSUserDefaults.standardUserDefaults setInteger:GB_PERCENT[i] forKey:@"gbSpeed"];
    } else {
        self.chipSpeed = i;
        chip8_scheduler_set_cpu_hz(&scheduler, CHIP_HZ[i]);
        [NSUserDefaults.standardUserDefaults setInteger:CHIP_HZ[i] forKey:@"cpuHz"];
    }
    [self.window makeFirstResponder:self.view];
}
- (void)demo:(id)sender {
    (void)sender; NSURL *url = [NSBundle.mainBundle URLForResource:@"nacre" withExtension:@"ch8"];
    if (url) [self loadURL:url];
}
- (void)tick:(NSTimer *)timer {
    (void)timer;
    uint64_t now = SDL_GetTicksNS(), elapsed = now - self.previous; self.previous = now;
    char error[256];
    if (!self.isGB) ffOn = NO;
    if (self.isGB) {
        [self pollPad];
        ffOn = (ffKey || (padHeld & PAD_BIT(GBI_FAST_FORWARD))) && ![self paused];
        unsigned percent = ffOn ? GB_FAST_PERCENT : GB_PERCENT[self.gbSpeed];
        if (![self paused] &&
            !gb_advance_ns(gb, elapsed, percent, error, sizeof(error))) {
            self.fault = [NSString stringWithUTF8String:error];
            [self writeSave]; [self releaseKeys];
            [self alert:self.fault]; [self updateControls];
        }
        [self syncAudio]; [self pumpGameBoyAudio];
        /* Write a battery save a second after the game last touched it. */
        if (self.savePath && gb_save_dirty(gb)) {
            if (!self.saveDue) self.saveDue = now + 1000000000ull;
            else if (now >= self.saveDue) [self writeSave];
        }
        self.view.needsDisplay = YES;
        return;
    }
    if (!chip8_scheduler_advance(&machine, &scheduler, elapsed, [self paused],
                               timer_tick, (__bridge void *)self, error, sizeof(error))) {
        self.fault = [NSString stringWithUTF8String:error]; [self releaseKeys];
        [self alert:self.fault]; [self updateControls];
    }
    [self syncAudio]; self.view.needsDisplay = YES;
}
- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification; self.focusPaused = YES; [self releaseKeys]; [self syncAudio];
}
- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification; self.focusPaused = NO; self.previous = SDL_GetTicksNS(); [self syncAudio];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender; return YES; }
- (BOOL)application:(NSApplication *)sender openFile:(NSString *)filename {
    (void)sender;
    NSURL *url = [NSURL fileURLWithPath:filename];
    if (!self.window) self.pendingURL = url;
    else [self loadURL:url];
    return YES;
}
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
    NSMenuItem *session = [bar addItemWithTitle:@"Session" action:NULL keyEquivalent:@""];
    session.submenu = [[NSMenu alloc] initWithTitle:@"Session"];
    [self addMenu:session.submenu title:@"Pause / Resume" action:@selector(togglePause:) key:@"p"];
    [self addMenu:session.submenu title:@"Restart ROM" action:@selector(resetROM:) key:@"r"];
    [self addMenu:session.submenu title:@"Toggle Sound" action:@selector(toggleSound:) key:@""];
    [session.submenu addItem:NSMenuItem.separatorItem];
    [self addMenu:session.submenu title:@"Save State" action:@selector(saveStateAction:) key:@"s"];
    [self addMenu:session.submenu title:@"Load State" action:@selector(loadStateAction:) key:@"l"];
    NSMenuItem *slots = [session.submenu addItemWithTitle:@"State Slot" action:NULL keyEquivalent:@""];
    slots.submenu = [[NSMenu alloc] initWithTitle:@"State Slot"];
    for (NSInteger i = 0; i < GB_STATE_SLOTS; i++) {
        NSMenuItem *item = [slots.submenu addItemWithTitle:[NSString stringWithFormat:@"Slot %ld", (long)i + 1]
                                                    action:@selector(chooseSlot:) keyEquivalent:@""];
        item.target = self; item.tag = i;
    }
    [session.submenu addItem:NSMenuItem.separatorItem];
    [self addMenu:session.submenu title:@"Controls…" action:@selector(showControls:) key:@","];
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
        @"Credits":[[NSAttributedString alloc] initWithString:@"A small CHIP-8 and Game Boy machine.\nMade by tdotlab."] }];
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    SDL_Init(SDL_INIT_AUDIO); chip8_init(&machine, 0x4E414352u);
    chip8_scheduler_init(&scheduler, CHIP8_DEFAULT_CPU_HZ);
    gb = gb_create();
    [self loadPrefs];
    if (!snapshotPath) pad_init();
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
    self.openButton.toolTip = @"Open a CHIP-8 or Game Boy ROM (⌘O)";
    self.pauseButton = [self button:@"Pause" action:@selector(togglePause:) symbol:@"pause.fill"];
    self.pauseButton.toolTip = @"Pause or resume (Space)";
    self.resetButton = [self button:@"Restart" action:@selector(resetROM:) symbol:@"arrow.counterclockwise"];
    self.resetButton.toolTip = @"Restart the loaded ROM (⌘R)";
    self.soundButton = [self button:@"Sound on" action:@selector(toggleSound:) symbol:@"speaker.wave.2"];
    self.soundButton.toolTip = @"Toggle sound (M)";
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    self.chipSpeed = 1; self.gbSpeed = 1;
    for (NSInteger i = 0; i < 3; i++) {
        if ([defaults integerForKey:@"cpuHz"] == (NSInteger)CHIP_HZ[i]) self.chipSpeed = i;
        if ([defaults integerForKey:@"gbSpeed"] == (NSInteger)GB_PERCENT[i]) self.gbSpeed = i;
    }
    self.speedButton = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [self.speedButton addItemsWithTitles:@[@"Relaxed · 300 Hz", @"Original · 600 Hz", @"Quick · 1000 Hz"]];
    [self.speedButton selectItemAtIndex:self.chipSpeed];
    self.speedButton.target = self; self.speedButton.action = @selector(changeSpeed:);
    [self.speedButton setAccessibilityLabel:@"Execution speed"]; [self.view addSubview:self.speedButton];
    self.paletteButton = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [self.paletteButton addItemsWithTitles:@[@"Fern", @"Amber", @"Pearl"]];
    self.palette = [defaults integerForKey:@"palette"];
    if (self.palette < 0 || self.palette > 2) self.palette = 0;
    [self.paletteButton selectItemAtIndex:self.palette];
    self.paletteButton.target = self; self.paletteButton.action = @selector(changePalette:);
    [self.paletteButton setAccessibilityLabel:@"Display palette"]; [self.view addSubview:self.paletteButton];
    self.muted = [defaults boolForKey:@"muted"];
    chip8_audio_oscillator_reset(&audio.oscillator);
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, CHIP8_AUDIO_SAMPLE_RATE};
    audio.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, &audio);
    self.audioAvailable = audio.stream && SDL_ResumeAudioStreamDevice(audio.stream);
    /* The Game Boy pushes its own stereo samples; the stream starts paused. */
    SDL_AudioSpec gbSpec = {SDL_AUDIO_S16, 2, (int)GB_AUDIO_SAMPLE_RATE};
    gbStream = gb ? SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &gbSpec, NULL, NULL) : NULL;
    self.view.needsLayout = YES;
    NSApp.applicationIconImage = NacreIcon();
    [self buildMenus]; [self.window center]; [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view]; [NSApp activateIgnoringOtherApps:YES];
    if (!self.pendingURL && launchPath) self.pendingURL = [NSURL fileURLWithPath:launchPath];
    if (self.pendingURL) { [self loadURL:self.pendingURL]; self.pendingURL = nil; }
    else [self demo:nil];
    [self updateControls];
    self.previous = SDL_GetTicksNS();
    self.timer = [NSTimer timerWithTimeInterval:1.0 / 60 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
    if (snapshotPath) [self performSelector:@selector(captureSnapshot) withObject:nil afterDelay:0.5];
}

/* A minimal 32 KB cartridge with a valid header, so the Game Boy path can be
 * checked without shipping a ROM. It waits for vblank, turns the LCD off, fills
 * the top half of tile 1 with the darkest shade, puts that tile in the map's top
 * left corner, turns the LCD back on and loops: the capture shows an 8x4 dark
 * block in the screen's top-left corner, which proves orientation. */
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
/* Presses and releases Right through the view's key path, then checks frames
 * advance. Returns 0, or base..base+2 for the failing step. */
- (int)checkGameBoyKeys:(int)base {
    NSString *right = [NSString stringWithFormat:@"%C", (unichar)NSRightArrowFunctionKey];
    NSEventModifierFlags flags = NSEventModifierFlagFunction | NSEventModifierFlagNumericPad;
    NSEvent *down = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
        modifierFlags:flags timestamp:0 windowNumber:self.window.windowNumber context:nil
        characters:right charactersIgnoringModifiers:right isARepeat:NO keyCode:124];
    [self.view keyDown:down];
    if (!gbHeld[GB_BUTTON_RIGHT]) return base;
    NSEvent *up = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint
        modifierFlags:flags timestamp:0 windowNumber:self.window.windowNumber context:nil
        characters:right charactersIgnoringModifiers:right isARepeat:NO keyCode:124];
    [self.view keyUp:up];
    if (gbHeld[GB_BUTTON_RIGHT]) return base + 1;
    char error[256];
    uint32_t frames = gb_frame_count(gb);
    if (!gb_run_cycles(gb, GB_CLOCK_HZ / 5, error, sizeof(error)) || gb_frame_count(gb) - frames < 10)
        return base + 2;
    return 0;
}
/* Fast-forward through the key path, then a save-state round trip in memory. Returns 0,
 * or base..base+3 for the failing step. */
- (int)checkGameBoyFeatures:(int)base {
    NSEvent *down = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:self.window.windowNumber context:nil
        characters:@"\t" charactersIgnoringModifiers:@"\t" isARepeat:NO keyCode:48];
    [self.view keyDown:down];
    if (!ffKey) return base;
    NSEvent *up = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:self.window.windowNumber context:nil
        characters:@"\t" charactersIgnoringModifiers:@"\t" isARepeat:NO keyCode:48];
    [self.view keyUp:up];
    if (ffKey) return base + 1;
    size_t size = gb_state_size(gb);
    NSMutableData *blob = [NSMutableData dataWithLength:size];
    if (size < 2 || gb_state_save(gb, blob.mutableBytes, size) != size) return base + 2;
    if (gb_state_load(gb, blob.bytes, size - 1)) return base + 3; /* a truncated state is refused */
    if (!gb_state_load(gb, blob.bytes, size)) return base + 3;
    [self applyButtons];
    return 0;
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
- (void)captureSnapshot {
    /* Exercise native actions and physical keypad dispatch before rendering. */
    BOOL originalMute = self.muted;
    [self togglePause:nil];
    if (!self.userPaused) { [self finishSnapshot:2]; return; }
    [self togglePause:nil];
    [self toggleSound:nil]; [self toggleSound:nil];
    if (self.muted != originalMute) { [self finishSnapshot:3]; return; }
    [self resetROM:nil];
    if (!self.romData || self.fault) { [self finishSnapshot:4]; return; }
    self.focusPaused = NO;
    char error[256];
    if (self.isGB) {
        /* Launched with a cartridge: check its keys, run it a while, and capture it. */
        int stage = [self checkGameBoyKeys:12];
        if (!stage) stage = [self checkGameBoyFeatures:16];
        if (stage) { [self finishSnapshot:stage]; return; }
        if (!gb_run_cycles(gb, (uint32_t)(GB_CLOCK_HZ * snapshotSeconds), error, sizeof(error))) {
            [self finishSnapshot:15]; return;
        }
        [self finishSnapshot:0];
        return;
    }
    NSEvent *down = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:self.window.windowNumber context:nil
        characters:@"x" charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:7];
    [self.view keyDown:down];
    if (!machine.keys[0]) { [self finishSnapshot:5]; return; }
    [self.view keyUp:down];
    if (machine.keys[0]) { [self finishSnapshot:6]; return; }

    /* The Game Boy path on a stand-in cartridge, then back to the CHIP-8 ROM. */
    NSData *rom = self.romData;
    NSURL *romURL = self.romURL;
    if (![self loadData:[self snapshotCartridge] url:nil] || !self.isGB || self.fault) {
        [self finishSnapshot:7]; return;
    }
    /* Let the stand-in finish drawing before counting frames. */
    if (!gb_run_cycles(gb, GB_CLOCK_HZ / 10, error, sizeof(error))) { [self finishSnapshot:7]; return; }
    int stage = [self checkGameBoyKeys:8];
    if (!stage) stage = [self checkGameBoyFeatures:20];
    if (stage) { [self finishSnapshot:stage]; return; }
    /* A second capture beside the first shows the Game Boy screen and keycaps. */
    NSString *gbShot = [snapshotPath.stringByDeletingPathExtension stringByAppendingString:@"-gb.png"];
    if (![self captureViewTo:gbShot]) { [self finishSnapshot:1]; return; }
    [self loadData:rom url:romURL];
    if (self.isGB) { [self finishSnapshot:11]; return; }

    chip8_scheduler_advance(&machine, &scheduler, (uint64_t)(snapshotSeconds * 1e9), 0, NULL, NULL,
                            error, sizeof(error));
    [self finishSnapshot:0];
}
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification; [self.timer invalidate]; SDL_SetAtomicInt(&audio.active, 0);
    [self writeSave];
    if (audio.stream) SDL_DestroyAudioStream(audio.stream);
    if (gbStream) SDL_DestroyAudioStream(gbStream);
    gbStream = NULL;
    if (gbContext) CGContextRelease(gbContext);
    gbContext = NULL;
    gb_destroy(gb); gb = NULL;
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
