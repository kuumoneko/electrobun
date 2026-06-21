import { ffi } from "../proc/native";
import electrobunEventEmitter from "../events/eventEmitter";
import { BrowserView } from "./BrowserView";
import { type Pointer } from "bun:ffi";
import { BuildConfig } from "./BuildConfig";
import { type RPCWithTransport } from "../../shared/rpc.js";
import { getNextWindowId } from "./windowIds";

const buildConfig = await BuildConfig.get();

export type WindowOptionsType<T = undefined> = {
	title: string;
	frame: {
		x: number;
		y: number;
		width: number;
		height: number;
	};
	url: string | null;
	html: string | null;
	preload: string | null;
	viewsRoot: string | null;
	renderer: "native";
	rpc?: T;
	styleMask?: {};
	// titleBarStyle options:
	// - 'default': normal titlebar with native window controls
	// - 'hidden': no titlebar, no native window controls (for fully custom chrome)
	// - 'hiddenInset': transparent titlebar with inset native controls
	titleBarStyle: "hidden" | "hiddenInset" | "default";
	// transparent: when true, window background is transparent (see-through)
	transparent: boolean;
	// passthrough: when true, mouse events pass through transparent regions
	passthrough: boolean;
	navigationRules: string | null;
	// Sandbox mode: when true, disables RPC and only allows event emission
	// Use for untrusted content (remote URLs) to prevent malicious sites from
	// accessing internal APIs, creating OOPIFs, or communicating with Bun
	sandbox: boolean;
};

const defaultOptions: WindowOptionsType = {
	title: "Electrobun",
	frame: {
		x: 0,
		y: 0,
		width: 800,
		height: 600,
	},
	url: "https://electrobun.dev",
	html: null,
	preload: null,
	viewsRoot: null,
	renderer: buildConfig.defaultRenderer,
	titleBarStyle: "default",
	transparent: false,
	passthrough: false,
	navigationRules: null,
	sandbox: false,
};

export const BrowserWindowMap: {
	[id: number]: BrowserWindow<RPCWithTransport>;
} = {};

// Clean up the window map when a window closes and optionally quit the app
electrobunEventEmitter.on("close", (event: { data: { id: number } }) => {
	const windowId = event.data.id;
	delete BrowserWindowMap[windowId];

	// Clean up all webviews associated with this window
	for (const view of BrowserView.getAll()) {
		if (view.windowId === windowId) {
			view.remove();
		}
	}
});

export class BrowserWindow<T extends RPCWithTransport = RPCWithTransport> {
	id: number = getNextWindowId();
	ptr!: Pointer;
	title: string = "Electrobun";
	state: "creating" | "created" = "creating";
	url: string | null = null;
	html: string | null = null;
	preload: string | null = null;
	viewsRoot: string | null = null;
	renderer: "native" = "native";
	transparent: boolean = false;
	passthrough: boolean = false;
	hidden: boolean = false;
	navigationRules: string | null = null;
	// Sandbox mode disables RPC and only allows event emission (for untrusted content)
	sandbox: boolean = false;
	frame: {
		x: number;
		y: number;
		width: number;
		height: number;
	} = {
			x: 0,
			y: 0,
			width: 800,
			height: 600,
		};
	// todo (yoav): make this an array of ids or something
	webviewId!: number;

	constructor(options: Partial<WindowOptionsType<T>> = defaultOptions) {
		this.title = options.title || "New Window";
		this.frame = options.frame
			? { ...defaultOptions.frame, ...options.frame }
			: { ...defaultOptions.frame };
		this.url = options.url || null;
		this.html = options.html || null;
		this.preload = options.preload || null;
		this.viewsRoot = options.viewsRoot || null;
		this.renderer = options.renderer || defaultOptions.renderer;
		this.transparent = options.transparent ?? false;
		this.passthrough = options.passthrough ?? false;
		this.navigationRules = options.navigationRules || null;
		this.sandbox = options.sandbox ?? false;

		this.init(options);
	}

	init({
		rpc,
		styleMask,
		titleBarStyle,
		transparent,
	}: Partial<WindowOptionsType<T>>) {
		this.ptr = ffi.request.createWindow({
			id: this.id,
			title: this.title,
			url: this.url || "",
			frame: {
				width: this.frame.width,
				height: this.frame.height,
				x: this.frame.x,
				y: this.frame.y,
			},
			styleMask: {
				Borderless: false,
				Titled: true,
				Closable: true,
				Miniaturizable: true,
				Resizable: true,
				UnifiedTitleAndToolbar: false,
				FullScreen: false,
				FullSizeContentView: false,
				UtilityWindow: false,
				DocModalWindow: false,
				NonactivatingPanel: false,
				HUDWindow: false,
				...(styleMask || {}),
				// hiddenInset: transparent titlebar with inset native controls
				...(titleBarStyle === "hiddenInset"
					? {
						Titled: true,
						FullSizeContentView: true,
					}
					: {}),
				// hidden: no titlebar, no native controls (for fully custom chrome)
				...(titleBarStyle === "hidden"
					? {
						Titled: false,
						FullSizeContentView: true,
					}
					: {}),
			},
			titleBarStyle: titleBarStyle || "default",
			transparent: transparent ?? false,
		}) as Pointer;

		BrowserWindowMap[this.id] = this;

		// todo (yoav): user should be able to override this and pass in their
		// own webview instance, or instances for attaching to the window.
		const webview = new BrowserView({
			// TODO: decide whether we want to keep sending url/html
			// here, if we're manually calling loadURL/loadHTML below
			// then we can remove it from the api here
			url: this.url,
			html: this.html,
			preload: this.preload,
			viewsRoot: this.viewsRoot,
			// frame: this.frame,
			renderer: this.renderer,
			frame: {
				x: 0,
				y: 0,
				width: this.frame.width,
				height: this.frame.height,
			},
			rpc,
			// todo: we need to send the window here and attach it in one go
			// then the view creation code in objc can toggle between offscreen
			// or on screen views depending on if windowId is null
			// does this mean browserView needs to track the windowId or handle it ephemerally?
			windowId: this.id,
			navigationRules: this.navigationRules,
			sandbox: this.sandbox,
			startPassthrough: this.passthrough,
		});

		this.webviewId = webview.id;
	}

	get webview() {
		return BrowserView.getById(this.webviewId) as BrowserView<T>;
	}

	static getById(id: number) {
		return BrowserWindowMap[id];
	}

	close() {
		return ffi.request.closeWindow({ winId: this.id });
	}

	focus() {
		return ffi.request.focusWindow({ winId: this.id });
	}

	show() {
		return ffi.request.focusWindow({ winId: this.id });
	}

	minimize() {
		return ffi.request.minimizeWindow({ winId: this.id });
	}

	maximize() {
		return ffi.request.maximizeWindow({ winId: this.id });
	}

	// todo (yoav): move this to a class that also has off, append, prepend, etc.
	// name should only allow browserWindow events
	on(name: string, handler: (event: unknown) => void) {
		const specificName = `${name}-${this.id}`;
		electrobunEventEmitter.on(specificName, handler);
	}
}
