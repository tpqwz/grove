#import <Cocoa/Cocoa.h>

#include <stddef.h>

/* The dock icon of a program that is not an application bundle. */
void grove_macos_icon(const void *bytes, size_t n) {
    NSData *data;
    NSImage *image;
    if (!bytes || n == 0 || n > 8u * 1024u * 1024u) {
        return;
    }
    data = [[NSData alloc] initWithBytes:bytes length:(NSUInteger)n];
    if (!data) {
        return;
    }
    image = [[NSImage alloc] initWithData:data];
    if (image) {
        [NSApp setApplicationIconImage:image];
        [image release];
    }
    [data release];
}
