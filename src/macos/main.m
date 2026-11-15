#import <Cocoa/Cocoa.h>
#import "branding.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include <time.h>
#include "chip8.h"
#include "scheduler.h"
#include "audio.h"
#include "gb.h"

static NSString *snapshotPath;
static NSString *launchPath;
static double snapshotSeconds = 0.2;
static int snapshotStatus;

static const uint32_t CHIP_HZ[3] = {300, 600, 1000};
static const unsigned GB_PERCENT[3] = {50, 100, 200};
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
/* Arrows for the D-pad, X and Z for A and B, Return for Start, Delete or the
 * right Shift for Select; physical key codes again. */
static int gb_button(unsigned short code) {
    switch (code) {
        case 124: return GB_BUTTON_RIGHT; case 123: return GB_BUTTON_LEFT;
        case 126: return GB_BUTTON_UP; case 125: return GB_BUTTON_DOWN;
        case 7: return GB_BUTTON_A; case 6: return GB_BUTTON_B;
        case 51: case 60: return GB_BUTTON_SELECT;
        case 36: case 76: return GB_BUTTON_START;
        default: return -1;
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
    uint8_t gbHeld[GB_BUTTON_COUNT];
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
- (void)drawGameBoyInWell:(NSRect)well;
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
        int button = gb_button(event.keyCode);
        if (button >= 0 && ![self.controller paused] && !event.isARepeat)
            [self.controller setGameBoyButton:button pressed:YES];
    } else {
        int key = chip_key(event.keyCode);
        if (key >= 0 && ![self.controller paused] && !event.isARepeat)
            chip8_set_key(&self.controller->machine, (uint8_t)key, 1);
    }
    self.needsDisplay = YES;
}
- (void)keyUp:(NSEvent *)event {
    if (self.controller.isGB) {
        int button = gb_button(event.keyCode);
        if (button >= 0) [self.controller setGameBoyButton:button pressed:NO];
    } else {
        int key = chip_key(event.keyCode);
        if (key >= 0) chip8_set_key(&self.controller->machine, (uint8_t)key, 0);
    }
    self.needsDisplay = YES;
}
- (void)flagsChanged:(NSEvent *)event {
    /* The right Shift is a modifier, so it arrives here rather than in keyDown.
     * 0x4 is the device-dependent right-Shift bit. */
    if (self.controller.isGB && event.keyCode == 60) {
        BOOL down = (event.modifierFlags & 0x4) != 0;
        if (!down || ![self.controller paused])
            [self.controller setGameBoyButton:GB_BUTTON_SELECT pressed:down];
        self.needsDisplay = YES;
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
        static const struct { int button; int col, row, cols; const char *label; CGFloat size; } caps[] = {
            {GB_BUTTON_UP, 1, 0, 1, "↑", 15},    {GB_BUTTON_LEFT, 0, 1, 1, "←", 15},
            {GB_BUTTON_RIGHT, 2, 1, 1, "→", 15}, {GB_BUTTON_DOWN, 1, 2, 1, "↓", 15},
            {GB_BUTTON_A, 3, 0, 1, "A", 12},     {GB_BUTTON_B, 3, 1, 1, "B", 12},
            {GB_BUTTON_SELECT, 0, 3, 2, "⌫ SELECT", 9}, {GB_BUTTON_START, 2, 3, 2, "↩ START", 9},
        };
        for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
            keycap(26 + caps[i].col * 44, 444 + caps[i].row * 39, caps[i].cols * 44 - 8,
                   [NSString stringWithUTF8String:caps[i].label], c->gbHeld[caps[i].button] != 0,
                   caps[i].size);
        text(@"Arrows  ·  X is A  ·  Z is B", NSMakeRect(26, 610, 190, 18), 10,
             NSFontWeightRegular, 0x7F847A, NO);
        text(@"Space to pause  ·  M to mute", NSMakeRect(26, 628, 190, 18), 10,
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
    fill(NSMakeRect(w - 133, 42, 6, 6), c.fault ? 0xB66C52 : paused ? 0xB09361 : 0x608C70, 3);
    text(c.fault ? @"STOPPED" : paused ? @"PAUSED" : @"RUNNING", NSMakeRect(w - 118, 37, 90, 20),
         10, NSFontWeightMedium, 0x6C786C, YES);

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
    if (paused || self.dropActive || c.fault) {
        NSString *badge = self.dropActive ? @"Drop to open ROM" : c.fault ? @"Execution stopped" : @"Paused";
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
    if (note) text(note, NSMakeRect(254, h - 34, w - 450, 18), 10, NSFontWeightRegular, 0x7A8276, NO);
    text(c.isGB ? @"59.7 HZ FRAMES" : @"60 HZ TIMERS", NSMakeRect(w - 146, h - 34, 120, 18), 9,
         NSFontWeightMedium, 0x87907F, YES);
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
- (void)setGameBoyButton:(int)button pressed:(BOOL)pressed {
    if (button < 0 || button >= GB_BUTTON_COUNT) return;
    gbHeld[button] = pressed ? 1 : 0;
    if (gb) gb_set_button(gb, (GbButton)button, pressed);
}
- (void)releaseKeys {
    for (uint8_t key = 0; key < 16; key++) chip8_set_key(&machine, key, 0);
    for (int button = 0; button < GB_BUTTON_COUNT; button++)
        if (gbHeld[button]) [self setGameBoyButton:button pressed:NO];
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
    if (self.isGB) {
        if (![self paused] &&
            !gb_advance_ns(gb, elapsed, GB_PERCENT[self.gbSpeed], error, sizeof(error))) {
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
