// file_dialog.h on macOS: NSOpenPanel, run modally on the main thread
// (where Lua, and so mep.open_file_dialog, runs).

#include "file_dialog.h"

#import <Cocoa/Cocoa.h>

namespace mep::file_dialog {

bool Available() { return true; }

std::string Open(const std::string &start_dir) {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.title = @"Open File";
        if (!start_dir.empty()) {
            panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:start_dir.c_str()]
                                            isDirectory:YES];
        }
        NSWindow *key = [NSApp keyWindow];
        const NSModalResponse r = [panel runModal];
        [key makeKeyAndOrderFront:nil];
        if (r != NSModalResponseOK || panel.URLs.count == 0) return "";
        const char *path = panel.URLs.firstObject.fileSystemRepresentation;
        return path ? std::string(path) : std::string();
    }
}

}  // namespace mep::file_dialog
