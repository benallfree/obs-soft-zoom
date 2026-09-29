#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import "zoom-outline.h"

uint32_t zoom_display_id_from_uuid(const char *uuid_str)
{
	if (!uuid_str || !*uuid_str)
		return kCGNullDirectDisplay;

	CFStringRef uuid_string = CFStringCreateWithCString(kCFAllocatorDefault, uuid_str, kCFStringEncodingUTF8);
	if (!uuid_string)
		return kCGNullDirectDisplay;

	CFUUIDRef target = CFUUIDCreateFromString(kCFAllocatorDefault, uuid_string);
	CFRelease(uuid_string);
	if (!target)
		return kCGNullDirectDisplay;

	uint32_t count = 0;
	CGGetActiveDisplayList(0, NULL, &count);
	if (!count) {
		CFRelease(target);
		return kCGNullDirectDisplay;
	}

	CGDirectDisplayID *displays = malloc(sizeof(CGDirectDisplayID) * count);
	CGGetActiveDisplayList(count, displays, &count);

	uint32_t match = kCGNullDirectDisplay;
	for (uint32_t i = 0; i < count; i++) {
		CFUUIDRef candidate = CGDisplayCreateUUIDFromDisplayID(displays[i]);
		if (candidate && CFEqual(candidate, target)) {
			match = displays[i];
			CFRelease(candidate);
			break;
		}
		if (candidate)
			CFRelease(candidate);
	}

	free(displays);
	CFRelease(target);
	return match;
}

bool zoom_display_frame_for_id(uint32_t display_id, zoom_display_frame *out)
{
	if (!out)
		return false;

	for (NSScreen *screen in NSScreen.screens) {
		NSNumber *num = screen.deviceDescription[@"NSScreenNumber"];
		if (!num || num.unsignedIntValue != display_id)
			continue;

		NSRect f = screen.frame;
		out->x = f.origin.x;
		out->y = f.origin.y;
		out->w = f.size.width;
		out->h = f.size.height;
		return true;
	}
	return false;
}

void zoom_cursor_normalized_on_display(uint32_t display_id, float *out_x, float *out_y, bool *on_display)
{
	zoom_display_frame frame = {0};
	if (!out_x || !out_y)
		return;

	*out_x = 0.5f;
	*out_y = 0.5f;
	if (on_display)
		*on_display = false;

	if (!zoom_display_frame_for_id(display_id, &frame))
		return;

	NSPoint p = [NSEvent mouseLocation];
	if (p.x < frame.x || p.x >= frame.x + frame.w || p.y < frame.y || p.y >= frame.y + frame.h) {
		return;
	}

	*out_x = (float)((p.x - frame.x) / frame.w);
	*out_y = (float)(1.0 - (p.y - frame.y) / frame.h);
	if (on_display)
		*on_display = true;
}

@interface ZoomOutlineView : NSView
@property(nonatomic) NSRect focusRect;
@property(nonatomic) NSInteger outlineThickness;
@property(nonatomic) NSInteger dimOpacity;
@end

@implementation ZoomOutlineView

- (BOOL)isFlipped
{
	return YES;
}

- (void)drawRect:(NSRect)dirtyRect
{
	(void)dirtyRect;

	NSRect focus = self.focusRect;
	if (focus.size.width <= 0 || focus.size.height <= 0)
		return;

	const CGFloat dimAlpha = (CGFloat)self.dimOpacity / 100.0f;
	if (dimAlpha > 0.001f) {
		[[NSColor colorWithWhite:0 alpha:dimAlpha] setFill];
		NSBezierPath *outer = [NSBezierPath bezierPathWithRect:self.bounds];
		NSBezierPath *inner = [NSBezierPath bezierPathWithRect:focus];
		[outer appendBezierPath:inner];
		[outer setWindingRule:NSWindingRuleEvenOdd];
		[outer fill];
	}

	if (self.outlineThickness > 0) {
		const CGFloat t = (CGFloat)self.outlineThickness;
		[[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.2 alpha:1] setFill];

		NSRect r = focus;
		// Top bar (outside)
		NSRectFill(NSMakeRect(r.origin.x - t, r.origin.y - t, r.size.width + 2 * t, t));
		// Bottom bar
		NSRectFill(NSMakeRect(r.origin.x - t, NSMaxY(r), r.size.width + 2 * t, t));
		// Left bar
		NSRectFill(NSMakeRect(r.origin.x - t, r.origin.y, t, r.size.height));
		// Right bar
		NSRectFill(NSMakeRect(NSMaxX(r), r.origin.y, t, r.size.height));
	}
}

@end

static NSPanel *g_panel;
static ZoomOutlineView *g_view;
static void zoom_outline_apply_on_main(const zoom_outline_params *params, bool show)
{
	if (!show || (params->outline_thickness <= 0 && params->dim_opacity <= 0)) {
		if (g_panel) {
			[g_panel orderOut:nil];
		}
		return;
	}

	NSRect screenFrame = NSMakeRect(params->display_x, params->display_y, params->display_w, params->display_h);

	if (!g_panel) {
		g_panel = [[NSPanel alloc] initWithContentRect:NSZeroRect
						     styleMask:NSWindowStyleMaskBorderless
						       backing:NSBackingStoreBuffered
							 defer:NO];
		g_panel.level = NSFloatingWindowLevel;
		g_panel.collectionBehavior =
			NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary |
			NSWindowCollectionBehaviorStationary;
		g_panel.backgroundColor = NSColor.clearColor;
		g_panel.opaque = NO;
		g_panel.hasShadow = NO;
		g_panel.ignoresMouseEvents = YES;
		g_view = [[ZoomOutlineView alloc] initWithFrame:NSZeroRect];
		g_panel.contentView = g_view;
	}

	[g_panel setFrame:screenFrame display:YES];

	const CGFloat fw = screenFrame.size.width;
	const CGFloat fh = screenFrame.size.height;

	g_view.frame = NSMakeRect(0, 0, fw, fh);
	g_view.focusRect =
		NSMakeRect((CGFloat)params->region_x * fw, (CGFloat)params->region_y * fh, (CGFloat)params->region_w * fw,
			   (CGFloat)params->region_h * fh);
	g_view.outlineThickness = params->outline_thickness;
	g_view.dimOpacity = params->dim_opacity;
	[g_view setNeedsDisplay:YES];

	[g_panel orderFrontRegardless];
}

static void dispatch_outline(const zoom_outline_params *params, bool show)
{
	zoom_outline_params copy = *params;
	dispatch_async(dispatch_get_main_queue(), ^{
		zoom_outline_apply_on_main(&copy, show);
	});
}

void zoom_outline_show(const zoom_outline_params *params)
{
	dispatch_outline(params, true);
}

void zoom_outline_update(const zoom_outline_params *params)
{
	dispatch_outline(params, true);
}

void zoom_outline_hide(void)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		if (g_panel)
			[g_panel orderOut:nil];
	});
}

void zoom_outline_shutdown(void)
{
	dispatch_sync(dispatch_get_main_queue(), ^{
		if (g_panel) {
			[g_panel close];
			g_panel = nil;
			g_view = nil;
		}
	});
}
