#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#include <stdlib.h>

int main(int argc, const char** argv) {
  @autoreleasepool {
    if(argc!=2 && argc!=3) {
      fprintf(stderr,"usage: window-id <owner-name> [owner-pid]\n");
      return 2;
      }

    NSString* target = [NSString stringWithUTF8String:argv[1]];
    const pid_t requestedPid = argc==3 ? (pid_t)strtol(argv[2],NULL,10) : 0;
    NSArray<NSDictionary*>* windows =
        CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionAll,
                                                      kCGNullWindowID));
    if([target isEqualToString:@"--list"]) {
      for(NSDictionary* window in windows) {
        const int layer = [window[(id)kCGWindowLayer] intValue];
        const int number = [window[(id)kCGWindowNumber] intValue];
        if(layer!=0 || number<=0)
          continue;
        NSString* owner = window[(id)kCGWindowOwnerName];
        NSString* title = window[(id)kCGWindowName];
        fprintf(stdout,"%d\t%s\t%s\n",
                number,owner.UTF8String ?: "",title.UTF8String ?: "");
        }
      return 0;
      }

    for(NSDictionary* window in windows) {
      NSString* owner = window[(id)kCGWindowOwnerName];
      NSString* title = window[(id)kCGWindowName];
      const pid_t ownerPid = [window[(id)kCGWindowOwnerPID] intValue];
      if(requestedPid>0 && ownerPid!=requestedPid)
        continue;
      const BOOL ownerMatches =
          owner!=nil &&
          [owner rangeOfString:target options:NSCaseInsensitiveSearch].location!=NSNotFound;
      const BOOL titleMatches =
          title!=nil &&
          [title rangeOfString:target options:NSCaseInsensitiveSearch].location!=NSNotFound;
      if(!ownerMatches && !titleMatches)
        continue;

      const int layer = [window[(id)kCGWindowLayer] intValue];
      const int number = [window[(id)kCGWindowNumber] intValue];
      if(layer==0 && number>0) {
        fprintf(stderr,"owner=%s pid=%d title=%s\n",
                owner.UTF8String ?: "",
                ownerPid,
                title.UTF8String ?: "");
        fprintf(stdout,"%d\n",number);
        return 0;
        }
      }

    fprintf(stderr,"unable to find window: %s\n",argv[1]);
    return 1;
    }
  }
