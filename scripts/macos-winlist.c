#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
int main(void){
  CFArrayRef l=CGWindowListCopyWindowInfo(kCGWindowListOptionAll,kCGNullWindowID);
  for(CFIndex i=0;i<CFArrayGetCount(l);i++){
    CFDictionaryRef d=CFArrayGetValueAtIndex(l,i); char o[256]="",n[256]="";
    CFStringRef s=CFDictionaryGetValue(d,kCGWindowOwnerName); if(s) CFStringGetCString(s,o,256,kCFStringEncodingUTF8);
    if(strcmp(o,"bb-probe")) continue;
    s=CFDictionaryGetValue(d,kCGWindowName); if(s) CFStringGetCString(s,n,256,kCFStringEncodingUTF8);
    int layer=0,id=0,on=0; CFNumberGetValue(CFDictionaryGetValue(d,kCGWindowLayer),kCFNumberIntType,&layer);
    CFNumberGetValue(CFDictionaryGetValue(d,kCGWindowNumber),kCFNumberIntType,&id);
    CFBooleanRef ob=CFDictionaryGetValue(d,kCGWindowIsOnscreen); on=ob&&CFBooleanGetValue(ob);
    CGRect r; CGRectMakeWithDictionaryRepresentation(CFDictionaryGetValue(d,kCGWindowBounds),&r);
    printf("id %d layer %d onscreen %d name '%s' %.0fx%.0f at %.0f,%.0f\n",id,layer,on,n,r.size.width,r.size.height,r.origin.x,r.origin.y);
  }
  return 0;}
