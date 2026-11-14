#import <Cocoa/Cocoa.h>
#import "../src/macos/branding.h"

/* An ICNS container of PNG representations, generated with system AppKit only. */
static void word(NSMutableData *data, uint32_t value) {
    uint32_t big = CFSwapInt32HostToBig(value);
    [data appendBytes:&big length:4];
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        [NSApplication sharedApplication];
        NSImage *icon = NacreIcon();
        NSMutableData *chunks = [NSMutableData data];
        const int sizes[] = {16,32,64,128,256,512,1024};
        const char *types[] = {"icp4","icp5","icp6","ic07","ic08","ic09","ic10"};
        for (int i = 0; i < 7; i++) {
            NSInteger size = sizes[i];
            NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                pixelsWide:size pixelsHigh:size bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
                isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
            [NSGraphicsContext saveGraphicsState];
            NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
            [icon drawInRect:NSMakeRect(0,0,size,size) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1];
            [NSGraphicsContext restoreGraphicsState];
            NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            if (!png) return 1;
            [chunks appendBytes:types[i] length:4]; word(chunks, (uint32_t)png.length + 8);
            [chunks appendData:png];
        }
        NSMutableData *icns = [NSMutableData dataWithBytes:"icns" length:4];
        word(icns, (uint32_t)chunks.length + 8); [icns appendData:chunks];
        return [icns writeToFile:[NSString stringWithUTF8String:argv[1]] atomically:YES] ? 0 : 1;
    }
}
