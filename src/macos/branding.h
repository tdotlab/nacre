#import <Cocoa/Cocoa.h>

/* Original shell mark, shared by the bundle icon tool and running Mac app. */
static NSColor *iconColor(unsigned rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0 green:((rgb >> 8) & 255) / 255.0 blue:(rgb & 255) / 255.0 alpha:1];
}
static void iconFill(NSRect rect, unsigned rgb, CGFloat radius) {
    [iconColor(rgb) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:rect xRadius:radius yRadius:radius] fill];
}
static NSImage *NacreIcon(void) {
    NSImage *icon = [[NSImage alloc] initWithSize:NSMakeSize(512, 512)];
    [icon lockFocus];
    iconFill(NSMakeRect(28, 28, 456, 456), 0x243E32, 100);
    [iconColor(0xCDDDC7) setStroke];
    for (int i = 0; i < 5; i++) {
        NSBezierPath *shell = [NSBezierPath bezierPath]; shell.lineWidth = 10;
        shell.lineCapStyle = NSRoundLineCapStyle;
        [shell moveToPoint:NSMakePoint(142 + i * 31, 146)];
        [shell curveToPoint:NSMakePoint(358, 330 - i * 29)
            controlPoint1:NSMakePoint(90 + i * 43, 380)
            controlPoint2:NSMakePoint(377, 407 - i * 26)];
        [shell stroke];
    }
    [icon unlockFocus];
    return icon;
}
