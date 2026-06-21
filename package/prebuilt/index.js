// src/bun/preload/encryption.ts
function base64ToUint8Array(base64) {
  return new Uint8Array(atob(base64).split("").map((char) => char.charCodeAt(0)));
}
function uint8ArrayToBase64(uint8Array) {
  let binary = "";
  for (let i = 0;i < uint8Array.length; i++) {
    binary += String.fromCharCode(uint8Array[i]);
  }
  return btoa(binary);
}
async function generateKeyFromBytes(rawKey) {
  return await window.crypto.subtle.importKey("raw", rawKey, { name: "AES-GCM" }, true, ["encrypt", "decrypt"]);
}
async function initEncryption() {
  const secretKey = await generateKeyFromBytes(new Uint8Array(window.__electrobunSecretKeyBytes));
  const encryptString = async (plaintext) => {
    const encoder = new TextEncoder;
    const encodedText = encoder.encode(plaintext);
    const iv = window.crypto.getRandomValues(new Uint8Array(12));
    const encryptedBuffer = await window.crypto.subtle.encrypt({ name: "AES-GCM", iv }, secretKey, encodedText);
    const encryptedData = new Uint8Array(encryptedBuffer.slice(0, -16));
    const tag = new Uint8Array(encryptedBuffer.slice(-16));
    return {
      encryptedData: uint8ArrayToBase64(encryptedData),
      iv: uint8ArrayToBase64(iv),
      tag: uint8ArrayToBase64(tag)
    };
  };
  const decryptString = async (encryptedDataB64, ivB64, tagB64) => {
    const encryptedData = base64ToUint8Array(encryptedDataB64);
    const iv = base64ToUint8Array(ivB64);
    const tag = base64ToUint8Array(tagB64);
    const combinedData = new Uint8Array(encryptedData.length + tag.length);
    combinedData.set(encryptedData);
    combinedData.set(tag, encryptedData.length);
    const decryptedBuffer = await window.crypto.subtle.decrypt({ name: "AES-GCM", iv }, secretKey, combinedData);
    const decoder = new TextDecoder;
    return decoder.decode(decryptedBuffer);
  };
  window.__electrobun_encrypt = encryptString;
  window.__electrobun_decrypt = decryptString;
}

// src/bun/preload/internalRpc.ts
var pendingRequests = {};
var requestId = 0;
var isProcessingQueue = false;
var sendQueue = [];
function processQueue() {
  if (isProcessingQueue) {
    setTimeout(processQueue);
    return;
  }
  if (sendQueue.length === 0)
    return;
  isProcessingQueue = true;
  const batch = JSON.stringify(sendQueue);
  sendQueue.length = 0;
  window.__electrobunInternalBridge?.postMessage(batch);
  setTimeout(() => {
    isProcessingQueue = false;
  }, 2);
}
function send(type, payload) {
  sendQueue.push(JSON.stringify({ type: "message", id: type, payload }));
  processQueue();
}
function request(type, payload) {
  return new Promise((resolve, reject) => {
    const id = `req_${++requestId}_${Date.now()}`;
    pendingRequests[id] = { resolve, reject };
    sendQueue.push(JSON.stringify({
      type: "request",
      method: type,
      id,
      params: payload,
      hostWebviewId: window.__electrobunWebviewId
    }));
    processQueue();
    setTimeout(() => {
      if (pendingRequests[id]) {
        delete pendingRequests[id];
        reject(new Error(`Request timeout: ${type}`));
      }
    }, 1e4);
  });
}
function handleResponse(msg) {
  if (msg && msg.type === "response" && msg.id) {
    const pending = pendingRequests[msg.id];
    if (pending) {
      delete pendingRequests[msg.id];
      if (msg.success)
        pending.resolve(msg.payload);
      else
        pending.reject(msg.payload);
    }
  }
}

// src/bun/preload/dragRegions.ts
function isAppRegionDrag(e) {
  const target = e.target;
  if (!target || !target.closest)
    return false;
  if (target.closest(".electrobun-webkit-app-region-no-drag") || target.closest('[style*="app-region"][style*="no-drag"]')) {
    return false;
  }
  const draggableByStyle = target.closest('[style*="app-region"][style*="drag"]');
  const draggableByClass = target.closest(".electrobun-webkit-app-region-drag");
  return !!(draggableByStyle || draggableByClass);
}
function initDragRegions() {
  document.addEventListener("mousedown", (e) => {
    if (isAppRegionDrag(e)) {
      send("startWindowMove", { id: window.__electrobunWindowId });
    }
  });
  document.addEventListener("mouseup", (e) => {
    if (isAppRegionDrag(e)) {
      send("stopWindowMove", { id: window.__electrobunWindowId });
    }
  });
}

// src/bun/preload/overlaySync.ts
class OverlaySyncController {
  element;
  options;
  lastRect = { x: 0, y: 0, width: 0, height: 0 };
  resizeObserver = null;
  positionLoop = null;
  resizeHandler = null;
  burstUntil = 0;
  constructor(element, options) {
    this.element = element;
    this.options = {
      onSync: options.onSync,
      getMasks: options.getMasks ?? (() => []),
      burstIntervalMs: options.burstIntervalMs ?? 50,
      baseIntervalMs: options.baseIntervalMs ?? 100,
      burstDurationMs: options.burstDurationMs ?? 500
    };
  }
  start() {
    this.resizeObserver = new ResizeObserver(() => this.sync());
    this.resizeObserver.observe(this.element);
    const loop = () => {
      this.sync();
      const now = performance.now();
      const interval = now < this.burstUntil ? this.options.burstIntervalMs : this.options.baseIntervalMs;
      this.positionLoop = setTimeout(loop, interval);
    };
    this.positionLoop = setTimeout(loop, this.options.baseIntervalMs);
    this.resizeHandler = () => this.sync(true);
    window.addEventListener("resize", this.resizeHandler);
  }
  stop() {
    if (this.resizeObserver)
      this.resizeObserver.disconnect();
    if (this.positionLoop)
      clearTimeout(this.positionLoop);
    if (this.resizeHandler) {
      window.removeEventListener("resize", this.resizeHandler);
    }
    this.resizeObserver = null;
    this.positionLoop = null;
    this.resizeHandler = null;
  }
  forceSync() {
    this.sync(true);
  }
  setLastRect(rect) {
    this.lastRect = rect;
  }
  sync(force = false) {
    const rect = this.element.getBoundingClientRect();
    const newRect = {
      x: rect.x,
      y: rect.y,
      width: rect.width,
      height: rect.height
    };
    if (newRect.width === 0 && newRect.height === 0) {
      return;
    }
    if (!force && newRect.x === this.lastRect.x && newRect.y === this.lastRect.y && newRect.width === this.lastRect.width && newRect.height === this.lastRect.height) {
      return;
    }
    this.burstUntil = performance.now() + this.options.burstDurationMs;
    this.lastRect = newRect;
    const masks = this.options.getMasks();
    this.options.onSync(newRect, JSON.stringify(masks));
  }
}

// src/bun/preload/webviewTag.ts
var webviewRegistry = {};

class ElectrobunWebviewTag extends HTMLElement {
  webviewId = null;
  maskSelectors = new Set;
  _sync = null;
  transparent = false;
  passthroughEnabled = false;
  hidden = false;
  sandboxed = false;
  _eventListeners = {};
  constructor() {
    super();
  }
  connectedCallback() {
    requestAnimationFrame(() => this.initWebview());
  }
  disconnectedCallback() {
    if (this.webviewId !== null) {
      send("webviewTagRemove", { id: this.webviewId });
      delete webviewRegistry[this.webviewId];
    }
    if (this._sync)
      this._sync.stop();
  }
  async initWebview() {
    const rect = this.getBoundingClientRect();
    const initialRect = {
      x: rect.x,
      y: rect.y,
      width: rect.width,
      height: rect.height
    };
    const url = this.getAttribute("src");
    const html = this.getAttribute("html");
    const preload = this.getAttribute("preload");
    const partition = this.getAttribute("partition");
    const renderer = this.getAttribute("renderer") || "native";
    const masks = this.getAttribute("masks");
    const sandbox = this.hasAttribute("sandbox");
    this.sandboxed = sandbox;
    const transparent = this.hasAttribute("transparent");
    const passthrough = this.hasAttribute("passthrough");
    this.transparent = transparent;
    this.passthroughEnabled = passthrough;
    if (transparent)
      this.style.opacity = "0";
    if (passthrough)
      this.style.pointerEvents = "none";
    if (masks) {
      masks.split(",").forEach((s) => this.maskSelectors.add(s.trim()));
    }
    try {
      const webviewId = await request("webviewTagInit", {
        hostWebviewId: window.__electrobunWebviewId,
        windowId: window.__electrobunWindowId,
        renderer,
        url,
        html,
        preload,
        partition,
        frame: {
          width: rect.width,
          height: rect.height,
          x: rect.x,
          y: rect.y
        },
        navigationRules: null,
        sandbox,
        transparent,
        passthrough
      });
      this.webviewId = webviewId;
      this.id = `electrobun-webview-${webviewId}`;
      webviewRegistry[webviewId] = this;
      this.setupObservers(initialRect);
      this.syncDimensions(true);
      requestAnimationFrame(() => {
        Object.values(webviewRegistry).forEach((webview) => {
          if (webview !== this && webview.webviewId !== null) {
            webview.syncDimensions(true);
          }
        });
      });
    } catch (err) {
      console.error("Failed to init webview:", err);
    }
  }
  setupObservers(initialRect) {
    const getMasks = () => {
      const rect = this.getBoundingClientRect();
      const masks = [];
      this.maskSelectors.forEach((selector) => {
        try {
          document.querySelectorAll(selector).forEach((el) => {
            const mr = el.getBoundingClientRect();
            masks.push({
              x: mr.x - rect.x,
              y: mr.y - rect.y,
              width: mr.width,
              height: mr.height
            });
          });
        } catch (_e) {}
      });
      return masks;
    };
    this._sync = new OverlaySyncController(this, {
      onSync: (rect, masksJson) => {
        if (this.webviewId === null)
          return;
        send("webviewTagResize", {
          id: this.webviewId,
          frame: rect,
          masks: masksJson
        });
      },
      getMasks,
      burstIntervalMs: 10,
      baseIntervalMs: 100,
      burstDurationMs: 50
    });
    this._sync.setLastRect(initialRect);
    this._sync.start();
  }
  syncDimensions(force = false) {
    if (!this._sync)
      return;
    if (force) {
      this._sync.forceSync();
    }
  }
  loadURL(url) {
    if (this.webviewId === null)
      return;
    this.setAttribute("src", url);
    send("webviewTagUpdateSrc", { id: this.webviewId, url });
  }
  loadHTML(html) {
    if (this.webviewId === null)
      return;
    send("webviewTagUpdateHtml", { id: this.webviewId, html });
  }
  reload() {
    if (this.webviewId !== null)
      send("webviewTagReload", { id: this.webviewId });
  }
  goBack() {
    if (this.webviewId !== null)
      send("webviewTagGoBack", { id: this.webviewId });
  }
  goForward() {
    if (this.webviewId !== null)
      send("webviewTagGoForward", { id: this.webviewId });
  }
  async canGoBack() {
    if (this.webviewId === null)
      return false;
    return await request("webviewTagCanGoBack", {
      id: this.webviewId
    });
  }
  async canGoForward() {
    if (this.webviewId === null)
      return false;
    return await request("webviewTagCanGoForward", {
      id: this.webviewId
    });
  }
  toggleTransparent(value) {
    if (this.webviewId === null)
      return;
    this.transparent = value !== undefined ? value : !this.transparent;
    this.style.opacity = this.transparent ? "0" : "";
    send("webviewTagSetTransparent", {
      id: this.webviewId,
      transparent: this.transparent
    });
  }
  togglePassthrough(value) {
    if (this.webviewId === null)
      return;
    this.passthroughEnabled = value !== undefined ? value : !this.passthroughEnabled;
    this.style.pointerEvents = this.passthroughEnabled ? "none" : "";
    send("webviewTagSetPassthrough", {
      id: this.webviewId,
      enablePassthrough: this.passthroughEnabled
    });
  }
  toggleHidden(value) {
    if (this.webviewId === null)
      return;
    this.hidden = value !== undefined ? value : !this.hidden;
    send("webviewTagSetHidden", { id: this.webviewId, hidden: this.hidden });
  }
  addMaskSelector(selector) {
    this.maskSelectors.add(selector);
    this.syncDimensions(true);
  }
  removeMaskSelector(selector) {
    this.maskSelectors.delete(selector);
    this.syncDimensions(true);
  }
  setNavigationRules(rules) {
    if (this.webviewId !== null) {
      send("webviewTagSetNavigationRules", { id: this.webviewId, rules });
    }
  }
  findInPage(searchText, options) {
    if (this.webviewId === null)
      return;
    const forward = options?.forward !== false;
    const matchCase = options?.matchCase || false;
    send("webviewTagFindInPage", {
      id: this.webviewId,
      searchText,
      forward,
      matchCase
    });
  }
  stopFindInPage() {
    if (this.webviewId !== null)
      send("webviewTagStopFind", { id: this.webviewId });
  }
  openDevTools() {
    if (this.webviewId !== null)
      send("webviewTagOpenDevTools", { id: this.webviewId });
  }
  closeDevTools() {
    if (this.webviewId !== null)
      send("webviewTagCloseDevTools", { id: this.webviewId });
  }
  toggleDevTools() {
    if (this.webviewId !== null)
      send("webviewTagToggleDevTools", { id: this.webviewId });
  }
  executeJavascript(js) {
    if (this.webviewId === null)
      return;
    send("webviewTagExecuteJavascript", { id: this.webviewId, js });
  }
  on(event, listener) {
    if (!this._eventListeners[event])
      this._eventListeners[event] = [];
    this._eventListeners[event].push(listener);
  }
  off(event, listener) {
    if (!this._eventListeners[event])
      return;
    const idx = this._eventListeners[event].indexOf(listener);
    if (idx !== -1)
      this._eventListeners[event].splice(idx, 1);
  }
  emit(event, detail) {
    const listeners = this._eventListeners[event];
    if (listeners) {
      const customEvent = new CustomEvent(event, { detail });
      listeners.forEach((fn) => fn(customEvent));
    }
  }
  get src() {
    return this.getAttribute("src");
  }
  set src(value) {
    if (value) {
      this.setAttribute("src", value);
      if (this.webviewId !== null)
        this.loadURL(value);
    } else {
      this.removeAttribute("src");
    }
  }
  get html() {
    return this.getAttribute("html");
  }
  set html(value) {
    if (value) {
      this.setAttribute("html", value);
      if (this.webviewId !== null)
        this.loadHTML(value);
    } else {
      this.removeAttribute("html");
    }
  }
  get preload() {
    return this.getAttribute("preload");
  }
  set preload(value) {
    if (value)
      this.setAttribute("preload", value);
    else
      this.removeAttribute("preload");
  }
  get renderer() {
    return this.getAttribute("renderer") || "native";
  }
  set renderer(value) {
    this.setAttribute("renderer", value);
  }
  get sandbox() {
    return this.sandboxed;
  }
}
function initWebviewTag() {
  if (!customElements.get("electrobun-webview")) {
    customElements.define("electrobun-webview", ElectrobunWebviewTag);
  }
  const injectStyles = () => {
    const style = document.createElement("style");
    style.textContent = `
electrobun-webview {
	display: block;
	width: 800px;
	height: 300px;
	background: #fff;
	background-repeat: no-repeat !important;
	overflow: hidden;
}
`;
    if (document.head?.firstChild) {
      document.head.insertBefore(style, document.head.firstChild);
    } else if (document.head) {
      document.head.appendChild(style);
    }
  };
  if (document.head) {
    injectStyles();
  } else {
    document.addEventListener("DOMContentLoaded", injectStyles);
  }
}

// src/bun/preload/events.ts
function emitWebviewEvent(eventName, detail) {
  setTimeout(() => {
    const bridge = window.__electrobunEventBridge || window.__electrobunInternalBridge;
    bridge?.postMessage(JSON.stringify({
      id: "webviewEvent",
      type: "message",
      payload: {
        id: window.__electrobunWebviewId,
        eventName,
        detail
      }
    }));
  });
}
function initLifecycleEvents() {
  window.addEventListener("load", () => {
    if (window === window.top) {
      emitWebviewEvent("dom-ready", document.location.href);
    }
  });
  window.addEventListener("popstate", () => {
    emitWebviewEvent("did-navigate-in-page", window.location.href);
  });
  window.addEventListener("hashchange", () => {
    emitWebviewEvent("did-navigate-in-page", window.location.href);
  });
}
var cmdKeyHeld = false;
var cmdKeyTimestamp = 0;
var CMD_KEY_THRESHOLD_MS = 500;
function isCmdHeld() {
  if (cmdKeyHeld)
    return true;
  return Date.now() - cmdKeyTimestamp < CMD_KEY_THRESHOLD_MS && cmdKeyTimestamp > 0;
}
function initCmdClickHandling() {
  window.addEventListener("keydown", (event) => {
    if (event.key === "Meta" || event.metaKey) {
      cmdKeyHeld = true;
      cmdKeyTimestamp = Date.now();
    }
  }, true);
  window.addEventListener("keyup", (event) => {
    if (event.key === "Meta") {
      cmdKeyHeld = false;
      cmdKeyTimestamp = Date.now();
    }
  }, true);
  window.addEventListener("blur", () => {
    cmdKeyHeld = false;
  });
  window.addEventListener("click", (event) => {
    if (event.metaKey || event.ctrlKey) {
      const anchor = event.target?.closest?.("a");
      if (anchor && anchor.href) {
        event.preventDefault();
        event.stopPropagation();
        event.stopImmediatePropagation();
        emitWebviewEvent("new-window-open", JSON.stringify({
          url: anchor.href,
          isCmdClick: true,
          isSPANavigation: false
        }));
      }
    }
  }, true);
}
function initSPANavigationInterception() {
  const originalPushState = history.pushState;
  const originalReplaceState = history.replaceState;
  history.pushState = function(state, title, url) {
    if (isCmdHeld() && url) {
      const resolvedUrl = new URL(String(url), window.location.href).href;
      emitWebviewEvent("new-window-open", JSON.stringify({
        url: resolvedUrl,
        isCmdClick: true,
        isSPANavigation: true
      }));
      return;
    }
    return originalPushState.apply(this, [state, title, url]);
  };
  history.replaceState = function(state, title, url) {
    if (isCmdHeld() && url) {
      const resolvedUrl = new URL(String(url), window.location.href).href;
      emitWebviewEvent("new-window-open", JSON.stringify({
        url: resolvedUrl,
        isCmdClick: true,
        isSPANavigation: true
      }));
      return;
    }
    return originalReplaceState.apply(this, [state, title, url]);
  };
}
function initOverscrollPrevention() {
  document.addEventListener("DOMContentLoaded", () => {
    const style = document.createElement("style");
    style.type = "text/css";
    style.appendChild(document.createTextNode("html, body { overscroll-behavior: none; }"));
    document.head.appendChild(style);
  });
}

// src/bun/preload/index.ts
initEncryption().catch((err) => console.error("Failed to initialize encryption:", err));
var internalMessageHandler = (msg) => {
  handleResponse(msg);
};
if (!window.__electrobun) {
  window.__electrobun = {
    receiveInternalMessageFromBun: internalMessageHandler,
    receiveMessageFromBun: (msg) => {
      console.log("receiveMessageFromBun (no handler):", msg);
    }
  };
} else {
  window.__electrobun.receiveInternalMessageFromBun = internalMessageHandler;
  window.__electrobun.receiveMessageFromBun = (msg) => {
    console.log("receiveMessageFromBun (no handler):", msg);
  };
}
window.__electrobunSendToHost = (message) => {
  emitWebviewEvent("host-message", JSON.stringify(message));
};
initLifecycleEvents();
initCmdClickHandling();
initSPANavigationInterception();
initOverscrollPrevention();
initDragRegions();
initWebviewTag();
