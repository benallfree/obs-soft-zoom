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
		NSRectFill(NSMakeRect(r.origin.x - t, r.origin.y - t, r.size.width + 2 * t, t));
		NSRectFill(NSMakeRect(r.origin.x - t, NSMaxY(r), r.size.width + 2 * t, t));
		NSRectFill(NSMakeRect(r.origin.x - t, r.origin.y, t, r.size.height));
		NSRectFill(NSMakeRect(NSMaxX(r), r.origin.y, t, r.size.height));
	}
}

@end

@interface ZoomOutlinePanel : NSObject
@property(nonatomic) NSPanel *panel;
@property(nonatomic) ZoomOutlineView *view;
@end

@implementation ZoomOutlinePanel
@end

static NSMutableDictionary<NSNumber *, ZoomOutlinePanel *> *g_panels;

static void ensure_panel_map(void)
{
	if (!g_panels)
		g_panels = [[NSMutableDictionary alloc] init];
}

static NSPanel *create_outline_panel(void)
{
	NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSZeroRect
						styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
						  backing:NSBackingStoreBuffered
						    defer:NO];
	panel.level = NSScreenSaverWindowLevel + 1;
	panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary |
				   NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
	panel.backgroundColor = NSColor.clearColor;
	panel.opaque = NO;
	panel.hasShadow = NO;
	panel.ignoresMouseEvents = YES;
	panel.hidesOnDeactivate = NO;
	return panel;
}

static void close_panel_entry(ZoomOutlinePanel *entry)
{
	if (!entry)
		return;
	if (entry.panel) {
		[entry.panel orderOut:nil];
		[entry.panel close];
		entry.panel = nil;
	}
	entry.view = nil;
}

static void zoom_outline_apply_on_main(zoom_outline_owner_id owner, const zoom_outline_params *params, bool show)
{
	ensure_panel_map();
	NSNumber *key = @(owner);

	if (!show || (params->outline_thickness <= 0 && params->dim_opacity <= 0)) {
		ZoomOutlinePanel *entry = g_panels[key];
		if (entry.panel)
			[entry.panel orderOut:nil];
		return;
	}

	ZoomOutlinePanel *entry = g_panels[key];
	if (!entry) {
		entry = [[ZoomOutlinePanel alloc] init];
		g_panels[key] = entry;
	}

	NSRect screenFrame = NSMakeRect(params->display_x, params->display_y, params->display_w, params->display_h);

	if (!entry.panel) {
		entry.panel = create_outline_panel();
		entry.view = [[ZoomOutlineView alloc] initWithFrame:NSZeroRect];
		entry.panel.contentView = entry.view;
	}

	[entry.panel setFrame:screenFrame display:YES];

	const CGFloat fw = screenFrame.size.width;
	const CGFloat fh = screenFrame.size.height;

	entry.view.frame = NSMakeRect(0, 0, fw, fh);
	entry.view.focusRect =
		NSMakeRect((CGFloat)params->region_x * fw, (CGFloat)params->region_y * fh, (CGFloat)params->region_w * fw,
			   (CGFloat)params->region_h * fh);
	entry.view.outlineThickness = params->outline_thickness;
	entry.view.dimOpacity = params->dim_opacity;
	[entry.view setNeedsDisplay:YES];

	[entry.panel orderFrontRegardless];
	[entry.panel setLevel:NSScreenSaverWindowLevel + 1];
}

static void dispatch_outline(zoom_outline_owner_id owner, const zoom_outline_params *params, bool show)
{
	zoom_outline_params copy = *params;
	dispatch_async(dispatch_get_main_queue(), ^{
		zoom_outline_apply_on_main(owner, &copy, show);
	});
}

void zoom_outline_show_for(zoom_outline_owner_id owner, const zoom_outline_params *params)
{
	dispatch_outline(owner, params, true);
}

void zoom_outline_update_for(zoom_outline_owner_id owner, const zoom_outline_params *params)
{
	dispatch_outline(owner, params, true);
}

static void hide_owner_on_main(zoom_outline_owner_id owner, bool close)
{
	ensure_panel_map();
	NSNumber *key = @(owner);
	ZoomOutlinePanel *entry = g_panels[key];
	if (!entry)
		return;
	if (close) {
		close_panel_entry(entry);
		[g_panels removeObjectForKey:key];
	} else if (entry.panel) {
		[entry.panel orderOut:nil];
	}
}

void zoom_outline_hide_for(zoom_outline_owner_id owner)
{
	if ([NSThread isMainThread]) {
		hide_owner_on_main(owner, false);
		return;
	}
	dispatch_async(dispatch_get_main_queue(), ^{
		hide_owner_on_main(owner, false);
	});
}

void zoom_outline_destroy_for(zoom_outline_owner_id owner)
{
	if ([NSThread isMainThread]) {
		hide_owner_on_main(owner, true);
	} else {
		dispatch_async(dispatch_get_main_queue(), ^{
			hide_owner_on_main(owner, true);
		});
	}
}

static void close_all_panels_on_main(void)
{
	if (!g_panels)
		return;
	for (NSNumber *key in g_panels.allKeys) {
		close_panel_entry(g_panels[key]);
	}
	[g_panels removeAllObjects];
}

void zoom_outline_shutdown(void)
{
	if ([NSThread isMainThread]) {
		close_all_panels_on_main();
	} else {
		dispatch_async(dispatch_get_main_queue(), ^{
			close_all_panels_on_main();
		});
	}
}
