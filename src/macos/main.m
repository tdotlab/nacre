#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include "chip8.h"
#include "scheduler.h"
#include "audio.h"

static NSString *snapshotPath;
static int snapshotStatus;

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
static void text(NSString *value, NSRect rect, CGFloat size, NSFontWeight weight,
                 unsigned rgb, BOOL mono) {
    NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
    style.lineBreakMode = NSLineBreakByTruncatingTail;
    NSFont *font = mono ? [NSFont monospacedSystemFontOfSize:size weight:weight]
                        : [NSFont systemFontOfSize:size weight:weight];
    [value drawInRect:rect withAttributes:@{ NSFontAttributeName:font,
        NSForegroundColorAttributeName:color(rgb), NSParagraphStyleAttributeName:style }];
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
}
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NacreView *view;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSURL *romURL;
@property(nonatomic, strong) NSData *romData;
@property(nonatomic, strong) NSString *fault;
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
    int key = chip_key(event.keyCode);
    if (key >= 0 && ![self.controller paused] && !event.isARepeat)
        chip8_set_key(&self.controller->machine, (uint8_t)key, 1);
    self.needsDisplay = YES;
}
- (void)keyUp:(NSEvent *)event {
    int key = chip_key(event.keyCode);
    if (key >= 0) chip8_set_key(&self.controller->machine, (uint8_t)key, 0);
    self.needsDisplay = YES;
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
    NSFont *wordmark = [NSFont fontWithName:@"Georgia" size:31] ?: [NSFont systemFontOfSize:31];
    [@"nacre" drawAtPoint:NSMakePoint(66, 22) withAttributes:@{
        NSFontAttributeName:wordmark, NSForegroundColorAttributeName:color(0x293D34)}];
    text(@"A LITTLE MACHINE, WELL KEPT.", NSMakeRect(26, 68, 184, 18), 8.5,
         NSFontWeightMedium, 0x788078, YES);
    text(@"SESSION", NSMakeRect(26, 114, 174, 18), 10, NSFontWeightSemibold, 0x83867D, YES);
    text(@"Drop a ROM anywhere to begin.", NSMakeRect(26, 192, 178, 32), 11,
         NSFontWeightRegular, 0x747B72, NO);
    fill(NSMakeRect(26, 232, 170, 1), 0xD8D8CF, 0);
    text(@"Execution speed", NSMakeRect(26, 249, 174, 20), 12, NSFontWeightMedium, 0x4A544B, NO);
    text(@"Display", NSMakeRect(26, 325, 174, 20), 12, NSFontWeightMedium, 0x4A544B, NO);
    text(@"KEYBOARD", NSMakeRect(26, 414, 174, 18), 10, NSFontWeightSemibold, 0x83867D, YES);
    const char *host = "1234QWERASDFZXCV";
    const int keys[16] = {1,2,3,12,4,5,6,13,7,8,9,14,10,0,11,15};
    for (int i = 0; i < 16; i++) {
        CGFloat x = 26 + (i % 4) * 44, y = 444 + (i / 4) * 39;
        BOOL held = c->machine.keys[keys[i]] != 0;
        fill(NSMakeRect(x, y + 2, 36, 31), held ? 0x416452 : 0xD9DAD1, 6);
        fill(NSMakeRect(x, y, 36, 30), held ? 0x527A64 : 0xF6F5F0, 6);
        text([NSString stringWithFormat:@"%c", host[i]], NSMakeRect(x + 12, y + 6, 22, 20),
             12, NSFontWeightMedium, held ? 0xFFFFFF : 0x596259, YES);
    }
    text(@"Space to pause  ·  M to mute", NSMakeRect(26, 610, 180, 18), 10,
         NSFontWeightRegular, 0x7F847A, NO);
    text(@"CHIP-8  /  64 × 32", NSMakeRect(26, h - 36, 180, 18), 10,
         NSFontWeightMedium, 0x878C80, YES);

    NSString *name = c.romURL ? c.romURL.lastPathComponent.stringByDeletingPathExtension : @"Your next little world";
    text(name, NSMakeRect(254, 27, w - 430, 32), 23, NSFontWeightSemibold, 0x293D34, NO);
    text(c.romURL ? @"CHIP-8 ROM" : @"Open a ROM or try the bundled demo", NSMakeRect(255, 63, w - 400, 18),
         10, NSFontWeightMedium, 0x7B8479, YES);
    BOOL paused = [c paused];
    fill(NSMakeRect(w - 133, 42, 6, 6), c.fault ? 0xB66C52 : paused ? 0xB09361 : 0x608C70, 3);
    text(c.fault ? @"STOPPED" : paused ? @"PAUSED" : @"RUNNING", NSMakeRect(w - 118, 37, 90, 20),
         10, NSFontWeightMedium, 0x6C786C, YES);

    NSRect well = NSMakeRect(254, 111, w - 286, h - 233);
    fill(NSOffsetRect(well, 0, 3), 0xD7D8CE, 14);
    fill(well, self.dropActive ? 0x294C3D : 0x18211E, 14);
    unsigned inks[] = {0xC3DCC7, 0xEBD8AF, 0xE8E9DF};
    CGFloat scale = floor(MIN((well.size.width - 48) / 64, (well.size.height - 64) / 32));
    CGFloat ox = floor(NSMidX(well) - 32 * scale), oy = floor(NSMidY(well) - 16 * scale);
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    [color(inks[c.palette]) setFill];
    for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x++)
        if (c->machine.display[y * 64 + x]) NSRectFill(NSMakeRect(ox + x * scale, oy + y * scale, scale, scale));
    [[NSGraphicsContext currentContext] setShouldAntialias:YES];
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
    text(c.fault ?: (c.audioAvailable ? @"Pixel perfect. Sound on your terms." : @"Audio unavailable — visual playback is ready."),
         NSMakeRect(254, h - 34, w - 450, 18), 10, NSFontWeightRegular, 0x7A8276, NO);
    text(@"60 HZ TIMERS", NSMakeRect(w - 146, h - 34, 120, 18), 9,
         NSFontWeightMedium, 0x87907F, YES);
}
@end

@implementation NacreController
- (BOOL)paused { return self.userPaused || self.focusPaused || self.dialogOpen || self.fault != nil; }
- (void)syncAudio {
    SDL_SetAtomicInt(&audio.active, self.audioAvailable && !self.muted && ![self paused] && machine.sound_timer > 0);
}
- (void)releaseKeys {
    for (uint8_t key = 0; key < 16; key++) chip8_set_key(&machine, key, 0);
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
    self.soundButton.enabled = self.audioAvailable;
    [self.soundButton setAccessibilityLabel:self.soundButton.title];
    self.view.needsDisplay = YES;
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
- (void)loadURL:(NSURL *)url {
    NSError *readError = nil;
    NSNumber *size = nil;
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:&readError];
    if (!size || size.unsignedLongLongValue == 0 || size.unsignedLongLongValue > CHIP8_MEMORY_SIZE - CHIP8_PROGRAM_START) {
        [self alert:@"Choose a non-empty CHIP-8 ROM no larger than 3,584 bytes. Your current session has been kept."]; return;
    }
    NSData *data = [NSData dataWithContentsOfURL:url options:0 error:&readError];
    Chip8 candidate; chip8_init(&candidate, 0x4E414352u);
    char error[256];
    if (!data || !chip8_load_bytes(&candidate, data.bytes, data.length, error, sizeof(error))) {
        [self alert:data ? [NSString stringWithUTF8String:error] : readError.localizedDescription]; return;
    }
    machine = candidate; self.romURL = url; self.romData = data; self.fault = nil;
    chip8_scheduler_init(&scheduler, (uint32_t)self.speedButton.selectedItem.tag);
    self.userPaused = NO; self.previous = SDL_GetTicksNS();
    [NSDocumentController.sharedDocumentController noteNewRecentDocumentURL:url];
    [self.window makeFirstResponder:self.view]; [self syncAudio]; [self updateControls];
}
- (void)openROM:(id)sender {
    (void)sender; if (self.dialogOpen) return;
    self.dialogOpen = YES; [self releaseKeys]; [self syncAudio];
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Open a CHIP-8 ROM"; panel.prompt = @"Open ROM";
    panel.allowsMultipleSelection = NO; panel.canChooseDirectories = NO;
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"ch8"] ?: UTTypeData,
        [UTType typeWithFilenameExtension:@"rom"] ?: UTTypeData, UTTypeData];
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
    Chip8 candidate; chip8_init(&candidate, 0x4E414352u); char error[256];
    if (!chip8_load_bytes(&candidate, self.romData.bytes, self.romData.length, error, sizeof(error))) return;
    machine = candidate; self.fault = nil; self.userPaused = NO;
    chip8_scheduler_init(&scheduler, (uint32_t)self.speedButton.selectedItem.tag);
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
    (void)sender; chip8_scheduler_set_cpu_hz(&scheduler, (uint32_t)self.speedButton.selectedItem.tag);
    [NSUserDefaults.standardUserDefaults setInteger:self.speedButton.selectedItem.tag forKey:@"cpuHz"];
    [self.window makeFirstResponder:self.view];
}
- (void)demo:(id)sender {
    (void)sender; NSURL *url = [NSBundle.mainBundle URLForResource:@"hello" withExtension:@"ch8"];
    if (url) [self loadURL:url];
}
- (void)tick:(NSTimer *)timer {
    (void)timer;
    uint64_t now = SDL_GetTicksNS(), elapsed = now - self.previous; self.previous = now;
    char error[256];
    if (!chip8_scheduler_advance(&machine, &scheduler, elapsed, [self paused],
                               NULL, NULL, error, sizeof(error))) {
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
    if (!self.window) return NO;
    [self loadURL:[NSURL fileURLWithPath:filename]]; return YES;
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
    [self addMenu:session.submenu title:@"Toggle Sound" action:@selector(toggleSound:) key:@"m"];
    NSMenuItem *view = [bar addItemWithTitle:@"View" action:NULL keyEquivalent:@""];
    view.submenu = [[NSMenu alloc] initWithTitle:@"View"];
    [self addMenu:view.submenu title:@"Toggle Full Screen" action:@selector(fullscreen:) key:@"f"];
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
        @"Credits":[[NSAttributedString alloc] initWithString:@"A small CHIP-8 machine.\nMade by tdotlab."] }];
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    SDL_Init(SDL_INIT_AUDIO); chip8_init(&machine, 0x4E414352u);
    chip8_scheduler_init(&scheduler, CHIP8_DEFAULT_CPU_HZ);
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
    self.openButton.contentTintColor = color(0x355A43); self.openButton.toolTip = @"Open a CHIP-8 ROM (⌘O)";
    self.pauseButton = [self button:@"Pause" action:@selector(togglePause:) symbol:@"pause.fill"];
    self.pauseButton.toolTip = @"Pause or resume (Space)";
    self.resetButton = [self button:@"Restart" action:@selector(resetROM:) symbol:@"arrow.counterclockwise"];
    self.resetButton.toolTip = @"Restart the loaded ROM (⌘R)";
    self.soundButton = [self button:@"Sound on" action:@selector(toggleSound:) symbol:@"speaker.wave.2"];
    self.soundButton.toolTip = @"Toggle sound (M)";
    self.speedButton = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [self.speedButton addItemsWithTitles:@[@"Relaxed · 300 Hz", @"Original · 600 Hz", @"Quick · 1000 Hz"]];
    const NSInteger speeds[] = {300,600,1000};
    NSInteger savedSpeed = [NSUserDefaults.standardUserDefaults integerForKey:@"cpuHz"];
    [self.speedButton selectItemAtIndex:1];
    for (NSInteger i = 0; i < 3; i++) {
        [self.speedButton itemAtIndex:i].tag = speeds[i];
        if (savedSpeed == speeds[i]) [self.speedButton selectItemAtIndex:i];
    }
    self.speedButton.target = self; self.speedButton.action = @selector(changeSpeed:);
    [self.speedButton setAccessibilityLabel:@"Execution speed"]; [self.view addSubview:self.speedButton];
    self.paletteButton = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [self.paletteButton addItemsWithTitles:@[@"Fern", @"Amber", @"Pearl"]];
    self.palette = [NSUserDefaults.standardUserDefaults integerForKey:@"palette"];
    if (self.palette < 0 || self.palette > 2) self.palette = 0;
    [self.paletteButton selectItemAtIndex:self.palette];
    self.paletteButton.target = self; self.paletteButton.action = @selector(changePalette:);
    [self.paletteButton setAccessibilityLabel:@"Display palette"]; [self.view addSubview:self.paletteButton];
    self.muted = [NSUserDefaults.standardUserDefaults boolForKey:@"muted"];
    chip8_audio_oscillator_reset(&audio.oscillator);
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, CHIP8_AUDIO_SAMPLE_RATE};
    audio.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, &audio);
    self.audioAvailable = audio.stream && SDL_ResumeAudioStreamDevice(audio.stream);
    [self buildMenus]; [self.window center]; [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view]; [NSApp activateIgnoringOtherApps:YES];
    [self demo:nil]; [self updateControls];
    self.previous = SDL_GetTicksNS();
    self.timer = [NSTimer timerWithTimeInterval:1.0 / 60 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
    if (snapshotPath) [self performSelector:@selector(captureSnapshot) withObject:nil afterDelay:0.5];
}
- (void)captureSnapshot {
    [self.view layoutSubtreeIfNeeded];
    [self.view displayIfNeeded];
    NSBitmapImageRep *bitmap = [self.view bitmapImageRepForCachingDisplayInRect:self.view.bounds];
    [self.view cacheDisplayInRect:self.view.bounds toBitmapImageRep:bitmap];
    NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    snapshotStatus = [png writeToFile:snapshotPath atomically:YES] ? 0 : 1;
    [NSApp terminate:nil];
}
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification; [self.timer invalidate]; SDL_SetAtomicInt(&audio.active, 0);
    if (audio.stream) SDL_DestroyAudioStream(audio.stream);
    SDL_Quit();
}
@end

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 3 && strcmp(argv[1], "--snapshot") == 0)
            snapshotPath = [NSString stringWithUTF8String:argv[2]];
        NSApplication *app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NacreController *controller = [NacreController new];
        app.delegate = controller; [app run];
    }
    return snapshotStatus;
}
