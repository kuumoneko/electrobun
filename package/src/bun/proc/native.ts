import { join } from "path";
import electrobunEventEmitter from "../events/eventEmitter";
import ElectrobunEvent from "../events/event";
import { BrowserView } from "../core/BrowserView";
import { Tray } from "../core/Tray";
import { preloadScript } from "../preload/.generated/compiled";

// Menu data reference system to avoid serialization overhead
const menuDataRegistry = new Map<string, any>();
let menuDataCounter = 0;

function storeMenuData(data: any): string {
	const id = `menuData_${++menuDataCounter}`;
	menuDataRegistry.set(id, data);
	return id;
}

function getMenuData(id: string): any {
	return menuDataRegistry.get(id);
}

function clearMenuData(id: string): void {
	menuDataRegistry.delete(id);
}

// Shared methods for EB delimiter serialization/deserialization
const ELECTROBUN_DELIMITER = "|EB|";

function serializeMenuAction(action: string, data: any): string {
	const dataId = storeMenuData(data);
	return `${ELECTROBUN_DELIMITER}${dataId}|${action}`;
}

function deserializeMenuAction(encodedAction: string): {
	action: string;
	data: any;
} {
	let actualAction = encodedAction;
	let data = undefined;

	if (encodedAction.startsWith(ELECTROBUN_DELIMITER)) {
		const parts = encodedAction.split("|");
		if (parts.length >= 4) {
			// ['', 'EB', 'dataId', 'actualAction', ...]
			const dataId = parts[2]!;
			actualAction = parts.slice(3).join("|"); // Rejoin in case action contains |
			data = getMenuData(dataId);

			// Clean up data from registry after use
			clearMenuData(dataId);
		}
	}

	return { action: actualAction, data };
}

import {
	dlopen,
	suffix,
	JSCallback,
	CString,
	ptr,
	FFIType,
	type Pointer,
} from "bun:ffi";
import { BrowserWindow } from "../core/BrowserWindow";

function getWindowPtr(winId: number) {
	return (
		BrowserWindow.getById(winId)?.ptr ?? null
	);
}

export const native = (() => {
	try {
		// Use absolute path to native wrapper DLL to avoid working directory issues
		// On Windows shortcuts, the working directory may not be set correctly
		const nativeWrapperPath = join(process.cwd(), `libNativeWrapper.${suffix}`);
		return dlopen(nativeWrapperPath, {
			// window
			createWindowWithFrameAndStyleFromWorker: {
				// Pass each parameter individually
				args: [
					FFIType.u32, // windowId
					FFIType.f64,
					FFIType.f64, // x, y
					FFIType.f64,
					FFIType.f64, // width, height
					FFIType.u32, // styleMask
					FFIType.cstring, // titleBarStyle
					FFIType.bool, // transparent
					FFIType.function, // closeHandler
					FFIType.function, // moveHandler
					FFIType.function, // resizeHandler
					FFIType.function, // focusHandler
					FFIType.function, // blurHandler
					FFIType.function, // keyHandler
				],
				returns: FFIType.ptr,
			},
			setWindowTitle: {
				args: [
					FFIType.ptr, // window ptr
					FFIType.cstring, // title
				],
				returns: FFIType.void,
			},
			showWindow: {
				args: [
					FFIType.ptr, // window ptr
				],
				returns: FFIType.void,
			},
			closeWindow: {
				args: [
					FFIType.ptr, // window ptr
				],
				returns: FFIType.void,
			},
			minimizeWindow: {
				args: [FFIType.ptr],
				returns: FFIType.void,
			},
			maximizeWindow: {
				args: [FFIType.ptr],
				returns: FFIType.void,
			},
			// webview
			initWebview: {
				args: [
					FFIType.u32, // webviewId
					FFIType.ptr, // windowPtr
					FFIType.cstring, // renderer
					FFIType.cstring, // url
					FFIType.f64,
					FFIType.f64, // x, y
					FFIType.f64,
					FFIType.f64, // width, height
					FFIType.bool, // autoResize
					FFIType.cstring, // partition
					FFIType.function, // decideNavigation: *const fn (u32, [*:0]const u8) callconv(.C) bool,
					FFIType.function, // webviewEventHandler: *const fn (u32, [*:0]const u8, [*:0]const u8) callconv(.C) void,
					FFIType.function, // eventBridgeHandler: *const fn (u32, [*:0]const u8) callconv(.C) void (events only, always active)
					FFIType.function, // bunBridgePostmessageHandler: *const fn (u32, [*:0]const u8) callconv(.C) void (user RPC, disabled in sandbox)
					FFIType.function, // internalBridgeHandler: *const fn (u32, [*:0]const u8) callconv(.C) void (internal RPC, disabled in sandbox)
					FFIType.cstring, // electrobunPreloadScript
					FFIType.cstring, // customPreloadScript
					FFIType.cstring, // viewsRoot
					FFIType.bool, // transparent
					FFIType.bool, // sandbox - when true, bunBridge and internalBridge are not set up
				],
				returns: FFIType.ptr,
			},

			setNextWebviewFlags: {
				args: [
					FFIType.bool, // startTransparent
					FFIType.bool, // startPassthrough
				],
				returns: FFIType.void,
			},
			loadURLInWebView: {
				args: [FFIType.ptr, FFIType.cstring],
				returns: FFIType.void,
			},

			webviewRemove: {
				args: [FFIType.ptr],
				returns: FFIType.void,
			},

			evaluateJavaScriptWithNoCompletion: {
				args: [FFIType.ptr, FFIType.cstring],
				returns: FFIType.void,
			},
			webviewOpenDevTools: {
				args: [FFIType.ptr],
				returns: FFIType.void,
			},

			// Tray
			createTray: {
				args: [
					FFIType.u32, // id
					FFIType.cstring, // title
					FFIType.cstring, // pathToImage
					FFIType.bool, // isTemplate
					FFIType.u32, // width
					FFIType.u32, //height
					FFIType.function, // trayItemHandler
				],
				returns: FFIType.ptr,
			},
			setTrayTitle: {
				args: [FFIType.ptr, FFIType.cstring],
				returns: FFIType.void,
			},
			setTrayImage: {
				args: [FFIType.ptr, FFIType.cstring],
				returns: FFIType.void,
			},
			setTrayMenu: {
				args: [FFIType.ptr, FFIType.cstring],
				returns: FFIType.void,
			},
			removeTray: {
				args: [FFIType.ptr],
				returns: FFIType.void,
			},

			getPrimaryDisplay: {
				args: [],
				returns: FFIType.cstring,
			},
			openFileDialog: {
				args: [
					FFIType.cstring,
					FFIType.cstring,
					FFIType.int,
					FFIType.int,
					FFIType.int,
				],
				returns: FFIType.cstring,
			},
			showMessageBox: {
				args: [
					FFIType.cstring, // type
					FFIType.cstring, // title
					FFIType.cstring, // message
					FFIType.cstring, // detail
					FFIType.cstring, // buttons (comma-separated)
					FFIType.int, // defaultId
					FFIType.int, // cancelId
				],
				returns: FFIType.int,
			},
			// Window style utilities
			getWindowStyle: {
				args: [
					FFIType.bool,
					FFIType.bool,
					FFIType.bool,
					FFIType.bool,
					FFIType.bool,
					FFIType.bool,
					FFIType.bool,
					FFIType.bool,
					FFIType.bool,
					FFIType.bool,
					FFIType.bool,
					FFIType.bool,
				],
				returns: FFIType.u32,
			},
			// JSCallback utils for native code to use
			setJSUtils: {
				args: [
					FFIType.function, // get Mimetype from url/filename
					FFIType.function, // get html property from webview
				],
				returns: FFIType.void,
			},
			setWindowIcon: {
				args: [
					FFIType.ptr, // window pointer
					FFIType.cstring, // icon path
				],
				returns: FFIType.void,
			},
			killApp: {
				args: [],
				returns: FFIType.void,
			},
			stopEventLoop: {
				args: [],
				returns: FFIType.void,
			},
			waitForShutdownComplete: {
				args: [FFIType.i32],
				returns: FFIType.void,
			},
			forceExit: {
				args: [FFIType.i32],
				returns: FFIType.void,
			},
			setQuitRequestedHandler: {
				args: [FFIType.function],
				returns: FFIType.void,
			},
				// FFIFn: {
			//   args: [],
			//   returns: FFIType.void
			// },
		});
	} catch (err) {
		// FFI not available — running as a carrot inside Bunny Ears or in a build-only context.
		return null;
	}
})();

export const hasFFI = native !== null;

// PostMessage bridge for carrot workers (inter-carrot communication, host events).
// Created when __bunnyCarrotBootstrap exists, regardless of FFI availability.
class PostMessageBridge {
	private requestId = 0;
	private pendingRequests = new Map<number, {
		resolve: (value: unknown) => void;
		reject: (error: Error) => void;
	}>();
	private eventHandlers = new Map<string, Set<(payload: unknown) => void>>();

	constructor() {
		if (typeof self !== "undefined" && typeof self.addEventListener === "function") {
			self.addEventListener("message", (event: MessageEvent) => {
				this.handleMessage(event.data);
			});
		}
	}

	sendAction(action: string, payload?: unknown) {
		self.postMessage({ type: "action", action, payload });
	}

	requestHost<T = unknown>(method: string, params?: unknown): Promise<T> {
		const id = ++this.requestId;
		self.postMessage({ type: "host-request", requestId: id, method, params });
		return new Promise<T>((resolve, reject) => {
			this.pendingRequests.set(id, {
				resolve: (v) => resolve(v as T),
				reject,
			});
		});
	}

	on(name: string, handler: (payload: unknown) => void) {
		const handlers = this.eventHandlers.get(name) ?? new Set();
		handlers.add(handler);
		this.eventHandlers.set(name, handlers);
		return () => {
			handlers.delete(handler);
			if (handlers.size === 0) this.eventHandlers.delete(name);
		};
	}

	emit(name: string, payload: unknown) {
		this.eventHandlers.get(name)?.forEach((h) => {
			try { h(payload); } catch (e) { console.error(`[bridge] event handler failed: ${name}`, e); }
		});
	}

	private handleMessage(message: any) {
		if (!message || typeof message !== "object" || !("type" in message)) return;

		if (message.type === "host-response") {
			const pending = this.pendingRequests.get(message.requestId);
			if (!pending) return;
			this.pendingRequests.delete(message.requestId);
			if (message.success) {
				pending.resolve(message.payload);
			} else {
				pending.reject(new Error(message.error || "Host request failed"));
			}
		} else if (message.type === "event") {
			this.emit(message.name, message.payload);
		} else if (message.type === "init") {
			this.emit("init", message);
		}
	}
}

const isCarrotWorker = !!(globalThis as any).__bunnyCarrotBootstrap;
export const bridge: PostMessageBridge | null = isCarrotWorker ? new PostMessageBridge() : null;

// Proxy wrapper: routes ffi.request calls through FFI when available,
// or through the postMessage bridge when running as a carrot without FFI.
function createFfiRequestProxy(ffiRequest: Record<string, Function>): Record<string, Function> {
	if (hasFFI) return ffiRequest;

	return new Proxy(ffiRequest, {
		get(target, method: string) {
			if (typeof method !== "string") return target[method];
			return (params?: unknown) => bridge!.requestHost(method, params);
		},
	});
}

// const _callbacks: unknown[] = [];

// NOTE: Bun seems to hit limits on args or arg types. eg: trying to send 12 bools results
// in only about 8 going through then params after that. I think it may be similar to
// a zig bug I ran into last year. So check number of args in a signature when alignment issues occur.

// Non-null accessor for use inside _ffiImpl — these methods are only called when hasFFI is true.
const native_ = native!;

const _ffiImpl = {
	request: {
		createWindow: (params: {
			id: number;
			url: string | null;
			title: string;
			frame: {
				width: number;
				height: number;
				x: number;
				y: number;
			};
			styleMask: {
				Borderless: boolean;
				Titled: boolean;
				Closable: boolean;
				Miniaturizable: boolean;
				Resizable: boolean;
				UnifiedTitleAndToolbar: boolean;
				FullScreen: boolean;
				FullSizeContentView: boolean;
				UtilityWindow: boolean;
				DocModalWindow: boolean;
				NonactivatingPanel: boolean;
				HUDWindow: boolean;
			};
			titleBarStyle: string;
			transparent: boolean;
		}): FFIType.ptr => {
			const {
				id,
				url: _url,
				title,
				frame: { x, y, width, height },
				styleMask: {
					Borderless,
					Titled,
					Closable,
					Miniaturizable,
					Resizable,
					UnifiedTitleAndToolbar,
					FullScreen,
					FullSizeContentView,
					UtilityWindow,
					DocModalWindow,
					NonactivatingPanel,
					HUDWindow,
				},
				titleBarStyle,
				transparent,
			} = params;

			const styleMask = native_.symbols.getWindowStyle(
				Borderless,
				Titled,
				Closable,
				Miniaturizable,
				Resizable,
				UnifiedTitleAndToolbar,
				FullScreen,
				FullSizeContentView,
				UtilityWindow,
				DocModalWindow,
				NonactivatingPanel,
				HUDWindow,
			);

			const windowPtr = native_.symbols.createWindowWithFrameAndStyleFromWorker(
				id,
				// frame
				x,
				y,
				width,
				height,
				styleMask,
				// style
				toCString(titleBarStyle),
				transparent,
				// callbacks
				windowCloseCallback,
				windowMoveCallback,
				windowResizeCallback,
				windowFocusCallback,
				windowBlurCallback,
				windowKeyCallback,
			);

			if (!windowPtr) {
				throw "Failed to create window";
			}

			native_.symbols.setWindowTitle(windowPtr, toCString(title));

			return windowPtr;
		},
		setTitle: (params: { winId: number; title: string }) => {
			const { winId, title } = params;
			const windowPtr = getWindowPtr(winId);

			if (!windowPtr) {
				throw `Can't add webview to window. window no longer exists`;
			}

			native_.symbols.setWindowTitle(windowPtr, toCString(title));
		},

		closeWindow: (params: { winId: number }) => {
			const { winId } = params;
			const windowPtr = getWindowPtr(winId);

			if (!windowPtr) {
				// Window already closed — silently ignore the race condition
				return;
			}

			native_.symbols.closeWindow(windowPtr);
			// Note: Cleanup of BrowserWindowMap happens in the windowCloseCallback
		},

		focusWindow: (params: { winId: number }) => {
			const { winId } = params;
			const windowPtr = getWindowPtr(winId);

			if (!windowPtr) {
				throw `Can't focus window. Window no longer exists`;
			}

			native_.symbols.showWindow(windowPtr);
		},

		minimizeWindow: (params: { winId: number }) => {
			const { winId } = params;
			const windowPtr = getWindowPtr(winId);

			if (!windowPtr) {
				throw `Can't minimize window. Window no longer exists`;
			}

			native_.symbols.minimizeWindow(windowPtr);
		},
		maximizeWindow: (params: { winId: number }) => {
			const { winId } = params;
			const windowPtr = getWindowPtr(winId);

			if (!windowPtr) {
				throw `Can't maximize window. Window no longer exists`;
			}

			native_.symbols.maximizeWindow(windowPtr);
		},
		createWebview: (params: {
			id: number;
			windowId: number;
			renderer: "native";
			rpcPort: number;
			secretKey: string;
			hostWebviewId: number | null;
			pipePrefix: string;
			url: string | null;
			html: string | null;
			partition: string | null;
			preload: string | null;
			viewsRoot: string | null;
			frame: {
				x: number;
				y: number;
				width: number;
				height: number;
			};
			autoResize: boolean;
			navigationRules: string | null;
			sandbox: boolean;
			startTransparent: boolean;
			startPassthrough: boolean;
		}): FFIType.ptr => {
			const {
				id,
				windowId,
				renderer,
				rpcPort,
				secretKey,
				// hostWebviewId: number | null;
				// pipePrefix: string;
				url,
				// html: string | null;
				partition,
				preload,
				viewsRoot,
				frame: { x, y, width, height },
				autoResize,
				sandbox,
				startTransparent,
				startPassthrough,
			} = params;

			const parentWindow = BrowserWindow.getById(windowId);
			const windowPtr = parentWindow?.ptr;
			// Get transparent flag from parent window
			const transparent = parentWindow?.transparent ?? false;

			if (!windowPtr) {
				throw `Can't add webview to window. window no longer exists`;
			}

			// Dynamic setup per-webview (variables that change for each webview)
			// EventBridge is available for ALL webviews (including sandboxed) for event emission
			// InternalBridge and BunBridge are only available for trusted (non-sandboxed) webviews
			let dynamicPreload: string;
			let selectedPreloadScript: string;
			// Trusted webview: all bridges, full preload
			// Note: Check existing value first to preserve bridges already set by CEF's OnContextCreated
			dynamicPreload = `
window.__electrobunWebviewId = ${id};
window.__electrobunWindowId = ${windowId};
window.__electrobunRpcSocketPort = ${rpcPort};
window.__electrobunSecretKeyBytes = [${secretKey}];
window.__electrobunEventBridge = window.__electrobunEventBridge || window.webkit?.messageHandlers?.eventBridge || window.eventBridge || window.chrome?.webview?.hostObjects?.eventBridge;
window.__electrobunInternalBridge = window.__electrobunInternalBridge || window.webkit?.messageHandlers?.internalBridge || window.internalBridge || window.chrome?.webview?.hostObjects?.internalBridge;
window.__electrobunBunBridge = window.__electrobunBunBridge || window.webkit?.messageHandlers?.bunBridge || window.bunBridge || window.chrome?.webview?.hostObjects?.bunBridge;
window.addEventListener('contextmenu', (e) => { e.preventDefault(); }, false);
`;
			selectedPreloadScript = preloadScript;

			const electrobunPreload = dynamicPreload + selectedPreloadScript;

			const customPreload = preload;

			// Pre-set flags before initWebview (workaround for FFI param count limits)
			native_.symbols.setNextWebviewFlags(startTransparent, startPassthrough);
			const webviewPtr = native_.symbols.initWebview(
				id,
				windowPtr,
				toCString(renderer),
				toCString(url || ""),
				x,
				y,
				width,
				height,
				autoResize,
				toCString(partition || "persist:default"),
				webviewDecideNavigation,
				webviewEventJSCallback,
				eventBridgeHandler, // Event-only bridge (always active, for dom-ready, navigation, etc.)
				bunBridgePostmessageHandler, // User RPC bridge (disabled in sandbox mode)
				internalBridgeHandler, // Internal RPC bridge (disabled in sandbox mode)
				toCString(electrobunPreload),
				toCString(customPreload || ""),
				toCString(viewsRoot || ""),
				transparent,
				sandbox, // When true, bunBridge and internalBridge are not set up in native code
			);

			if (!webviewPtr) {
				throw "Failed to create webview";
			}

			return webviewPtr;
		},

		evaluateJavascriptWithNoCompletion: (params: {
			id: number;
			js: string;
		}) => {
			const { id, js } = params;
			const webview = BrowserView.getById(id);

			if (!webview?.ptr) {
				return;
			}

			native_.symbols.evaluateJavaScriptWithNoCompletion(
				webview.ptr,
				toCString(js),
			);
		},

		createTray: (params: {
			id: number;
			title: string;
			image: string;
			template: boolean;
			width: number;
			height: number;
		}): FFIType.ptr => {
			const { id, title, image, template, width, height } = params;

			const trayPtr = native_.symbols.createTray(
				id,
				toCString(title),
				toCString(image),
				template,
				width,
				height,
				trayItemHandler,
			);

			if (!trayPtr) {
				throw "Failed to create tray";
			}

			return trayPtr;
		},
		setTrayTitle: (params: { id: number; title: string }): void => {
			const { id, title } = params;

			const tray = Tray.getById(id);
			if (!tray) return;

			native_.symbols.setTrayTitle(tray.ptr, toCString(title));
		},
		setTrayImage: (params: { id: number; image: string }): void => {
			const { id, image } = params;

			const tray = Tray.getById(id);
			if (!tray) return;

			native_.symbols.setTrayImage(tray.ptr, toCString(image));
		},
		setTrayMenu: (params: {
			id: number;
			// json string of config
			menuConfig: string;
		}): void => {
			const { id, menuConfig } = params;

			const tray = Tray.getById(id);
			if (!tray) return;

			native_.symbols.setTrayMenu(tray.ptr, toCString(menuConfig));
		},

		removeTray: (params: { id: number }): void => {
			const { id } = params;
			const tray = Tray.getById(id);

			if (!tray) {
				throw `Can't remove tray. Tray no longer exists`;
			}

			native_.symbols.removeTray(tray.ptr);
			// The Tray class will handle removing from TrayMap
		},

		openFileDialog: (params: {
			startingFolder: string;
			allowedFileTypes: string;
			canChooseFiles: boolean;
			canChooseDirectory: boolean;
			allowsMultipleSelection: boolean;
		}): string => {
			const {
				startingFolder,
				allowedFileTypes,
				canChooseFiles,
				canChooseDirectory,
				allowsMultipleSelection,
			} = params;
			const filePath = native_.symbols.openFileDialog(
				toCString(startingFolder),
				toCString(allowedFileTypes),
				canChooseFiles ? 1 : 0,
				canChooseDirectory ? 1 : 0,
				allowsMultipleSelection ? 1 : 0,
			);

			return filePath.toString();
		},
		showMessageBox: (params: {
			type?: string;
			title?: string;
			message?: string;
			detail?: string;
			buttons?: string[];
			defaultId?: number;
			cancelId?: number;
		}): number => {
			const {
				type = "info",
				title = "",
				message = "",
				detail = "",
				buttons = ["OK"],
				defaultId = 0,
				cancelId = -1,
			} = params;
			// Convert buttons array to comma-separated string
			const buttonsStr = buttons.join(",");
			return native_.symbols.showMessageBox(
				toCString(type),
				toCString(title),
				toCString(message),
				toCString(detail),
				toCString(buttonsStr),
				defaultId,
				cancelId,
			);
		},
	},
	// Internal functions for menu data management
	internal: {
		storeMenuData,
		getMenuData,
		clearMenuData,
		serializeMenuAction,
		deserializeMenuAction,
	},
};

export const ffi = {
	request: createFfiRequestProxy(_ffiImpl.request as unknown as Record<string, Function>) as typeof _ffiImpl.request,
	internal: _ffiImpl.internal,
};

// Worker management. Move to a different file
process.on("uncaughtException", (err) => {
	console.error("Uncaught exception in worker:", err);
	if (native) {
		native_.symbols.stopEventLoop();
		native_.symbols.waitForShutdownComplete(5000);
		native_.symbols.forceExit(1);
	} else {
		process.exit(1);
	}
});

process.on("unhandledRejection", (reason, _promise) => {
	console.error("Unhandled rejection in worker:", reason);
});

process.on("SIGINT", () => {
	console.log("[electrobun] Received SIGINT, running quit sequence...");
	const { quit } = require("../core/Utils");
	quit();
});

process.on("SIGTERM", () => {
	console.log("[electrobun] Received SIGTERM, running quit sequence...");
	const { quit } = require("../core/Utils");
	quit();
});

const windowCloseCallback = new JSCallback(
	(id) => {
		const handler = electrobunEventEmitter.events.window.close;
		const event = handler({
			id,
		});

		// emit specific event first so user per-window handlers run
		// before the global handler (e.g. exitOnLastWindowClosed)
		electrobunEventEmitter.emitEvent(event, id);
		electrobunEventEmitter.emitEvent(event);
	},
	{
		args: ["u32"],
		returns: "void",
		threadsafe: true,
	},
);

const windowMoveCallback = new JSCallback(
	(id, x, y) => {
		const handler = electrobunEventEmitter.events.window.move;
		const event = handler({
			id,
			x,
			y,
		});

		// global event
		electrobunEventEmitter.emitEvent(event);
		electrobunEventEmitter.emitEvent(event, id);
	},
	{
		args: ["u32", "f64", "f64"],
		returns: "void",
		threadsafe: true,
	},
);

const windowResizeCallback = new JSCallback(
	(id, x, y, width, height) => {
		const handler = electrobunEventEmitter.events.window.resize;
		const event = handler({
			id,
			x,
			y,
			width,
			height,
		});

		// global event
		electrobunEventEmitter.emitEvent(event);
		electrobunEventEmitter.emitEvent(event, id);
	},
	{
		args: ["u32", "f64", "f64", "f64", "f64"],
		returns: "void",
		threadsafe: true,
	},
);

const windowFocusCallback = new JSCallback(
	(id) => {
		const handler = electrobunEventEmitter.events.window.focus;
		const event = handler({
			id,
		});

		// global event
		electrobunEventEmitter.emitEvent(event);
		electrobunEventEmitter.emitEvent(event, id);
	},
	{
		args: ["u32"],
		returns: "void",
		threadsafe: true,
	},
);

const windowBlurCallback = new JSCallback(
	(id) => {
		const handler = electrobunEventEmitter.events.window.blur;
		const event = handler({
			id,
		});

		// global event
		electrobunEventEmitter.emitEvent(event);
		electrobunEventEmitter.emitEvent(event, id);
	},
	{
		args: ["u32"],
		returns: "void",
		threadsafe: true,
	},
);

// global event
const windowKeyCallback = new JSCallback(
	(id, keyCode, modifiers, isDown, isRepeat) => {
		const handler = isDown
			? electrobunEventEmitter.events.window.keyDown
			: electrobunEventEmitter.events.window.keyUp;
		const event = handler({
			id,
			keyCode,
			modifiers,
			isRepeat: !!isRepeat,
		});
		electrobunEventEmitter.emitEvent(event);
		electrobunEventEmitter.emitEvent(event, id);
	},
	{
		args: ["u32", "u32", "u32", "u32", "u32"],
		returns: "void",
		threadsafe: true,
	},
);

const getMimeType = new JSCallback(
	(filePath) => {
		const _filePath = new CString(filePath).toString();
		const mimeType = Bun.file(_filePath).type; // || "application/octet-stream";

		// For this usecase we generally don't want the charset included in the mimetype
		// otherwise it can break. eg: for html with text/javascript;charset=utf-8 browsers
		// will tend to render the code/text instead of interpreting the html.

		return toCString(mimeType.split(";")[0]!);
	},
	{
		args: [FFIType.cstring],
		returns: FFIType.cstring,
		// threadsafe: true
	},
);

const getHTMLForWebviewSync = new JSCallback(
	(webviewId) => {
		const webview = BrowserView.getById(webviewId);

		return toCString(webview?.html || "");
	},
	{
		args: [FFIType.u32],
		returns: FFIType.cstring,
		// threadsafe: true
	},
);

if (native) native_.symbols.setJSUtils(getMimeType, getHTMLForWebviewSync);

// Types for Screen API
export interface Rectangle {
	x: number;
	y: number;
	width: number;
	height: number;
}

export interface Display {
	id: number;
	bounds: Rectangle;
	workArea: Rectangle;
	scaleFactor: number;
	isPrimary: boolean;
}

export interface Point {
	x: number;
	y: number;
}

// Screen module for display and cursor information
export const Screen = {
	/**
	 * Get the primary display
	 * @returns Display object for the primary monitor
	 */
	getPrimaryDisplay: (): Display => {
		const jsonStr = native ? native_.symbols.getPrimaryDisplay() : null;
		if (!jsonStr) {
			return {
				id: 0,
				bounds: { x: 0, y: 0, width: 0, height: 0 },
				workArea: { x: 0, y: 0, width: 0, height: 0 },
				scaleFactor: 1,
				isPrimary: true,
			};
		}
		try {
			return JSON.parse(jsonStr.toString());
		} catch {
			return {
				id: 0,
				bounds: { x: 0, y: 0, width: 0, height: 0 },
				workArea: { x: 0, y: 0, width: 0, height: 0 },
				scaleFactor: 1,
				isPrimary: true,
			};
		}
	},
};

// DEPRECATED: This callback is no longer used for navigation decisions.
// Navigation rules are now stored in native code and evaluated synchronously
// without calling back to Bun. Use webview.setNavigationRules() instead.
// This callback is kept for FFI signature compatibility but is not called.
const webviewDecideNavigation = new JSCallback(
	(_webviewId, _url) => {
		return true;
	},
	{
		args: [FFIType.u32, FFIType.cstring],
		returns: FFIType.u32,
		threadsafe: true,
	},
);

const webviewEventHandler = (id: number, eventName: string, detail: string) => {
	const webview = BrowserView.getById(id);
	if (!webview) {
		console.error("[webviewEventHandler] No webview found for id:", id);
		return;
	}

	if (webview.hostWebviewId) {
		const hostWebview = BrowserView.getById(webview.hostWebviewId);

		if (!hostWebview) {
			console.error("[webviewEventHandler] No webview found for id:", id);
			return;
		}

		// This is a webviewtag so we should send the event into the parent as well
		// NOTE: for new-window-open and host-message the detail is a json string that needs to be parsed
		let js;
		if (eventName === "new-window-open" || eventName === "host-message") {
			// detail is already a JSON string that will be parsed as a JS object
			js = `document.querySelector('#electrobun-webview-${id}').emit(${JSON.stringify(eventName)}, ${detail});`;
		} else {
			js = `document.querySelector('#electrobun-webview-${id}').emit(${JSON.stringify(eventName)}, ${JSON.stringify(detail)});`;
		}

		native_.symbols.evaluateJavaScriptWithNoCompletion(
			hostWebview.ptr,
			toCString(js),
		);
	}

	const eventMap: Record<string, string> = {
		"will-navigate": "willNavigate",
		"did-navigate": "didNavigate",
		"did-navigate-in-page": "didNavigateInPage",
		"did-commit-navigation": "didCommitNavigation",
		"dom-ready": "domReady",
		"new-window-open": "newWindowOpen",
		"host-message": "hostMessage",
		"download-started": "downloadStarted",
		"download-progress": "downloadProgress",
		"download-completed": "downloadCompleted",
		"download-failed": "downloadFailed",
		"load-started": "loadStarted",
		"load-committed": "loadCommitted",
		"load-finished": "loadFinished",
	};

	const mappedName = eventMap[eventName];
	const handler = mappedName
		? (electrobunEventEmitter.events.webview as Record<string, unknown>)[
		mappedName
		]
		: undefined;

	if (!handler) {
		// console.error(
		// 	"[webviewEventHandler] No handler found for event:",
		// 	eventName,
		// 	"(mapped to:",
		// 	mappedName,
		// 	")",
		// );
		return { success: false };
	}

	// Parse JSON data for events that send JSON
	let parsedDetail = detail;
	if (
		eventName === "new-window-open" ||
		eventName === "host-message" ||
		eventName === "download-started" ||
		eventName === "download-progress" ||
		eventName === "download-completed" ||
		eventName === "download-failed"
	) {
		try {
			parsedDetail = JSON.parse(detail);
		} catch (e) {
			console.error("[webviewEventHandler] Failed to parse JSON:", e);
			// Fallback to string if parsing fails (backward compatibility)
			parsedDetail = detail;
		}
	}

	const event = (
		handler as (data: { detail: string }) => ElectrobunEvent<unknown, unknown>
	)({
		detail: parsedDetail,
	});

	// global event
	electrobunEventEmitter.emitEvent(event);
	electrobunEventEmitter.emitEvent(event, id);
};

const webviewEventJSCallback = new JSCallback(
	(id, _eventName, _detail) => {
		let eventName = "";
		let detail = "";

		try {
			// Convert cstring pointers to actual strings
			eventName = new CString(_eventName).toString();
			detail = new CString(_detail).toString();
		} catch (err) {
			console.error("[webviewEventJSCallback] Error converting strings:", err);
			console.error("[webviewEventJSCallback] Raw values:", {
				_eventName,
				_detail,
			});
			return;
		}

		webviewEventHandler(id, eventName, detail);
	},
	{
		args: [FFIType.u32, FFIType.cstring, FFIType.cstring],
		returns: FFIType.void,
		threadsafe: true,
	},
);

const bunBridgePostmessageHandler = new JSCallback(
	(id, msg) => {
		try {
			const msgStr = new CString(msg);

			if (!msgStr.length) {
				return;
			}
			const rawMessage = msgStr.toString().trim();
			if (!rawMessage || (rawMessage[0] !== "{" && rawMessage[0] !== "[")) {
				return;
			}
			const msgJson = JSON.parse(rawMessage);

			const webview = BrowserView.getById(id);
			if (!webview) return;

			webview.rpcHandler?.(msgJson);
		} catch (err) {
			console.error("error sending message to bun: ", err);
		}
	},
	{
		args: [FFIType.u32, FFIType.cstring],
		returns: FFIType.void,
		threadsafe: true,
	},
);

// internalRPC (bun <-> browser internal stuff)
// BrowserView.rpc (user defined bun <-> browser rpc unique to each webview)
// nativeRPC (internal bun <-> native rpc)

// eventBridgeHandler: handles ONLY webview events (dom-ready, navigation, etc.)
// This is available on ALL webviews including sandboxed ones.
// It cannot process RPC requests - only event emission.
const eventBridgeHandler = new JSCallback(
	(_id: number, msg: number) => {
		try {
			const message = new CString(msg as unknown as Pointer);
			const rawMessage = message.toString().trim();
			if (!rawMessage || (rawMessage[0] !== "{" && rawMessage[0] !== "[")) {
				return;
			}
			const jsonMessage = JSON.parse(rawMessage);

			// Only handle webviewEvent messages - no RPC
			if (jsonMessage.id === "webviewEvent") {
				const { payload } = jsonMessage;
				webviewEventHandler(payload.id, payload.eventName, payload.detail);
			}
			// Silently ignore any other message types - sandboxed webviews shouldn't send them
		} catch (err) {
			console.error("error in eventBridgeHandler: ", err);
		}
	},
	{
		args: [FFIType.u32, FFIType.cstring],
		returns: FFIType.void,
		threadsafe: true,
	},
);

// internalBridgeHandler: handles internal RPC (webview tags, drag regions, etc.)
// This is only available on trusted (non-sandboxed) webviews.
const internalBridgeHandler = new JSCallback(
	(_id: number, msg: number) => {
		try {
			const batchMessage = new CString(msg as unknown as Pointer);
			const jsonBatch = JSON.parse(batchMessage.toString());

			if (jsonBatch.id === "webviewEvent") {
				// Note: Some WebviewEvents from inside the webview are routed through here
				// Others call the JSCallback directly from native code.
				const { payload } = jsonBatch;
				webviewEventHandler(payload.id, payload.eventName, payload.detail);
				return;
			}

			jsonBatch.forEach((msgStr: string) => {
				// if (!msgStr.length) {
				//   console.error('WEBVIEW EVENT SENT TO WEBVIEW TAG BRIDGE HANDLER?', )
				//   return;
				// }
				const msgJson = JSON.parse(msgStr);

				if (msgJson.type === "message") {
					const handler = (
						internalRpcHandlers.message as Record<
							string,
							(params: unknown) => void
						>
					)[msgJson.id];
					handler?.(msgJson.payload);
				} else if (msgJson.type === "request") {
					const hostWebview = BrowserView.getById(msgJson.hostWebviewId);
					// const targetWebview = BrowserView.getById(msgJson.params.params.hostWebviewId);
					const handler = (
						internalRpcHandlers.request as Record<
							string,
							(params: unknown) => unknown
						>
					)[msgJson.method];

					const payload = handler?.(msgJson.params);

					const resultObj = {
						type: "response",
						id: msgJson.id,
						success: true,
						payload,
					};

					if (!hostWebview) {
						console.log(
							"--->>> internal request in bun: NO HOST WEBVIEW FOUND",
						);
						return;
					}

					hostWebview.sendInternalMessageViaExecute(resultObj);
				}
			});
		} catch (err) {
			console.error("error in internalBridgeHandler: ", err);
			// console.log('msgStr: ', id, new CString(msg));
		}
	},
	{
		args: [FFIType.u32, FFIType.cstring],
		returns: FFIType.void,
		threadsafe: true,
	},
);

const trayItemHandler = new JSCallback(
	(id, action) => {
		// Note: Some invisible character that doesn't appear in .length
		// is causing issues
		const actionString = (new CString(action).toString() || "").trim();

		// Use shared deserialization method
		const { action: actualAction, data } = deserializeMenuAction(actionString);

		const event = electrobunEventEmitter.events.tray.trayClicked({
			id,
			action: actualAction,
			data, // Always include data property (undefined if no data)
		});

		// global event
		electrobunEventEmitter.emitEvent(event);
		electrobunEventEmitter.emitEvent(event, id);
	},
	{
		args: [FFIType.u32, FFIType.cstring],
		returns: FFIType.void,
		threadsafe: true,
	},
);

// Note: When passed over FFI JS will GC the buffer/pointer. Make sure to use strdup() or something
// on the c side to duplicate the string so objc/c++ gc can own it
export function toCString(
	jsString: string,
	addNullTerminator: boolean = true,
): CString {
	let appendWith = "";

	if (addNullTerminator && !jsString.endsWith("\0")) {
		appendWith = "\0";
	}
	const buff = Buffer.from(jsString + appendWith, "utf8");

	// @ts-ignore - This is valid in Bun
	return ptr(buff);
}

export const internalRpcHandlers = {
	request: {
	},
	message: {
		webviewTagExecuteJavascript: (params: { id: number; js: string }) => {
			const webview = BrowserView.getById(params.id);
			if (!webview || !webview.ptr) {
				console.error(
					`webviewTagExecuteJavascript: BrowserView not found or has no ptr for id ${params.id}`,
				);
				return;
			}
			native_.symbols.evaluateJavaScriptWithNoCompletion(
				webview.ptr,
				toCString(params.js),
			);
		},
		webviewEvent: (params: unknown) => {
			console.log("-----------------+webviewEvent", params);
		},
	},
};

// todo: consider renaming to TrayMenuItemConfig
export type MenuItemConfig =
	| { type: "divider" | "separator" }
	| {
		type: "normal";
		label: string;
		tooltip?: string;
		action?: string;
		data?: any;
		submenu?: Array<MenuItemConfig>;
		enabled?: boolean;
		checked?: boolean;
		hidden?: boolean;
	};

export type ApplicationMenuItemConfig =
	| { type: "divider" | "separator" }
	| {
		type?: "normal";
		label: string;
		tooltip?: string;
		action?: string;
		data?: any;
		submenu?: Array<ApplicationMenuItemConfig>;
		enabled?: boolean;
		checked?: boolean;
		hidden?: boolean;
		accelerator?: string;
	}
	| {
		type?: "normal";
		label?: string;
		tooltip?: string;
		role?: string;
		data?: any;
		submenu?: Array<ApplicationMenuItemConfig>;
		enabled?: boolean;
		checked?: boolean;
		hidden?: boolean;
		accelerator?: string;
	};
