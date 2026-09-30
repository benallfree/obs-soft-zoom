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
	const CGFloat right = frame.x + frame.w;
	const CGFloat top = frame.y + frame.h;
	if (p.x < frame.x || p.x > right || p.y < frame.y || p.y > top) {
		return;
	}

	const double nx = (p.x - frame.x) / frame.w;
	const double ny = 1.0 - (p.y - frame.y) / frame.h;
	*out_x = (float)(nx < 0.0 ? 0.0 : (nx > 1.0 ? 1.0 : nx));
	*out_y = (float)(ny < 0.0 ? 0.0 : (ny > 1.0 ? 1.0 : ny));
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

- (BOOL)isOpaque
{
	return NO;
}

- (void)drawRect:(NSRect)dirtyRect
{
	(void)dirtyRect;

	CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
	CGContextSaveGState(ctx);
	CGContextSetBlendMode(ctx, kCGBlendModeCopy);
	CGContextClearRect(ctx, NSRectToCGRect(self.bounds));
	CGContextRestoreGState(ctx);

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
static zoom_outline_params g_pending;
static bool g_pending_show;
static bool g_update_queued;

static void zoom_outline_apply_on_main(const zoom_outline_params *params, bool show)
{
	if (!show || (params->outline_thickness <= 0 && params->dim_opacity <= 0)) {
		if (g_panel) {
			[g_panel orderOut:nil];
		}
		return;
	}

	const CGFloat fw = params->display_w;
	const CGFloat fh = params->display_h;
	const CGFloat rx = params->region_x * fw;
	const CGFloat ry = params->region_y * fh;
	const CGFloat rw = params->region_w * fw;
	const CGFloat rh = params->region_h * fh;
	const CGFloat t = params->outline_thickness > 0 ? (CGFloat)params->outline_thickness : 0;
	const bool dimmed = params->dim_opacity > 0;

	NSRect panelFrame;
	NSRect focus;
	if (dimmed) {
		panelFrame = NSMakeRect(params->display_x, params->display_y, fw, fh);
		focus = NSMakeRect(rx, ry, rw, rh);
	} else {
		const CGFloat focusY = params->display_y + fh - (ry + rh);
		panelFrame = NSMakeRect(params->display_x + rx - t, focusY - t, rw + 2 * t, rh + 2 * t);
		focus = NSMakeRect(t, t, rw, rh);
	}

	if (panelFrame.size.width < 1 || panelFrame.size.height < 1)
		return;

	if (!g_panel) {
		g_panel = [[NSPanel alloc] initWithContentRect:NSZeroRect
						     styleMask:NSWindowStyleMaskBorderless |
							     NSWindowStyleMaskNonactivatingPanel
						       backing:NSBackingStoreBuffered
							 defer:NO];
		// Above normal app windows while OBS is in the background (not NSFloatingWindowLevel).
		g_panel.level = NSScreenSaverWindowLevel + 1;
		g_panel.collectionBehavior =
			NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary |
			NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
		g_panel.backgroundColor = NSColor.clearColor;
		g_panel.opaque = NO;
		g_panel.hasShadow = NO;
		g_panel.ignoresMouseEvents = YES;
		g_panel.hidesOnDeactivate = NO;
		if ([g_panel respondsToSelector:@selector(setSharingType:)])
			g_panel.sharingType = NSWindowSharingNone;
		g_view = [[ZoomOutlineView alloc] initWithFrame:NSZeroRect];
		g_view.wantsLayer = YES;
		g_view.layer.opaque = NO;
		g_view.layer.backgroundColor = NSColor.clearColor.CGColor;
		g_panel.contentView = g_view;
	}

	[g_panel setFrame:panelFrame display:NO];
	g_view.frame = NSMakeRect(0, 0, panelFrame.size.width, panelFrame.size.height);
	g_view.focusRect = focus;
	g_view.outlineThickness = params->outline_thickness;
	g_view.dimOpacity = params->dim_opacity;
	[g_view setNeedsDisplay:YES];
	[g_view displayIfNeeded];

	[g_panel orderFrontRegardless];
	[g_panel displayIfNeeded];
	// Reassert stacking after other apps take focus (orderFrontRegardless alone can lose to full-screen apps).
	[g_panel setLevel:NSScreenSaverWindowLevel + 1];
}

static void flush_pending_outline(void)
{
	g_update_queued = false;
	zoom_outline_apply_on_main(&g_pending, g_pending_show);
}

static void dispatch_outline(const zoom_outline_params *params, bool show)
{
	g_pending = *params;
	g_pending_show = show;
	if (g_update_queued)
		return;

	g_update_queued = true;
	dispatch_async(dispatch_get_main_queue(), ^{
		flush_pending_outline();
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

static void close_panel_on_main(void)
{
	if (!g_panel)
		return;
	[g_panel orderOut:nil];
	[g_panel close];
	g_panel = nil;
	g_view = nil;
}

void zoom_outline_hide(void)
{
	if ([NSThread isMainThread]) {
		if (g_panel)
			[g_panel orderOut:nil];
		return;
	}
	dispatch_async(dispatch_get_main_queue(), ^{
		if (g_panel)
			[g_panel orderOut:nil];
	});
}

void zoom_outline_shutdown(void)
{
	// Never dispatch_sync to main from OBS filter destroy: quit often blocks the main
	// thread until destroy returns, which deadlocks if we wait on the main queue.
	if ([NSThread isMainThread]) {
		close_panel_on_main();
	} else {
		dispatch_async(dispatch_get_main_queue(), ^{
			close_panel_on_main();
		});
	}
}
