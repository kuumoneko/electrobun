import { readFileSync, writeFileSync } from "fs";

const path = "src/native/macos/nativeWrapper.mm";
const src = readFileSync(path, "utf8");
const lines = src.split("\n");

// Comment out a range of lines [startIdx, endIdx] (0-indexed inclusive)
function commentRange(start, end) {
  for (let i = start; i <= end && i < lines.length; i++) {
    const trimmed = lines[i].trim();
    if (trimmed && !trimmed.startsWith("//") && !trimmed.startsWith("/*")) {
      lines[i] = "// " + lines[i];
    }
  }
}

// ===== Individual includes =====
for (let i = 0; i < lines.length; i++) {
  const l = lines[i];
  // WGPU include
  if (l.includes('#include "dawn/webgpu.h"')) {
    lines[i] = "// " + l;
  }
  // CEF includes block (lines 36-53)
  if (l === "// CEF includes") {
    lines[i] = "// " + l;
    for (let j = i + 1; j <= i + 17 && j < lines.length; j++) {
      if (lines[j].trim().startsWith("#include ")) {
        lines[j] = "// " + lines[j];
      }
    }
  }
}

// ===== wgpuDebugEnabled() (lines 20-26) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].trim() === "static bool wgpuDebugEnabled() {") {
    let j = i;
    while (j < lines.length && lines[j].trim() !== "}") j++;
    if (j < lines.length) j++; // include the closing brace line
    commentRange(i, j);
    break;
  }
}

// ===== CEF forward declarations + RemoteDevToolsClient (lines 312-350) =====
commentRange(311, 349); // 0-indexed: line 312 → index 311

// ===== RemoteDevToolsWindowDelegate (lines 352-368) =====
commentRange(351, 367);

// ===== isCEFAvailable() (lines 526-531) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].trim() === "bool isCEFAvailable() {") {
    let j = i;
    while (j < lines.length && lines[j].trim() !== "}") j++;
    if (j < lines.length) j++;
    commentRange(i, j);
    break;
  }
}

// ===== CEFOSRView @interface (lines 792-807) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("@interface CEFOSRView : NSView")) {
    let j = i;
    while (j < lines.length && !lines[j].trim().startsWith("@end")) j++;
    if (j < lines.length) j++;
    commentRange(i, j - 1);
    break;
  }
}

// ===== WGPUViewImpl @interface (lines 869-874) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("@interface WGPUViewImpl : AbstractView")) {
    let j = i;
    while (j < lines.length && !lines[j].trim().startsWith("@end")) j++;
    if (j < lines.length) j++;
    commentRange(i, j - 1);
    break;
  }
}

// ===== ElectrobunNSApplication @interface (lines 880-884) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("@interface ElectrobunNSApplication : NSApplication")) {
    let j = i;
    while (j < lines.length && !lines[j].trim().startsWith("@end")) j++;
    if (j < lines.length) j++;
    commentRange(i, j - 1);
    break;
  }
}

// ===== CEFOSRView @implementation (lines ~1520-1899) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("@implementation CEFOSRView")) {
    let j = i;
    while (j < lines.length && !lines[j].trim().startsWith("@end")) j++;
    if (j < lines.length) j++;
    commentRange(i, j - 1);
    break;
  }
}

// ===== WGPUViewImpl section (lines ~2909-3109) =====
// Covers: WGPUViewImpl comment, WGPUInputView @interface/@impl, WGPUViewImpl @impl
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("// ----------------------- WGPUViewImpl -----------------------")) {
    let j = i;
    // Find the next major section or @end
    while (j < lines.length) {
      const t = lines[j].trim();
      if ((t.startsWith("// --") && t !== lines[i].trim()) ||
          (j > i && t.startsWith("// -------") && !t.includes("WGPUViewImpl"))) {
        break;
      }
      j++;
    }
    commentRange(i, j - 1);
    // Also include the blank line before the next section if it exists
    break;
  }
}

// Find where AbstractView's @end is (before WGPU section) to know the exact range end
// Actually let me just use a simpler approach for the WGPU block
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("// ----------------------- WGPU Main-Thread Shims -----------------------")) {
    // Find the end - the next major section comment
    let j = i + 1;
    while (j < lines.length) {
      const t = lines[j].trim();
      if (t.startsWith("// --") && !t.includes("WGPU Main-Thread") && !t.includes("WGPUViewImpl")) {
        break;
      }
      j++;
    }
    commentRange(i, j - 1);
    break;
  }
}

// ===== WGPU GPU test section =====
// Find "// ----------------------- WGPU Native Test (macOS) -----------------------"
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("WGPU Native Test")) {
    // Find the end - the next major section (CEF and NSApplication Setup)
    let j = i + 1;
    while (j < lines.length) {
      const t = lines[j].trim();
      if (t.includes("CEF and NSApplication Setup")) break;
      j++;
    }
    commentRange(i, j - 1);
    break;
  }
}

// ===== CEF and NSApplication Setup (lines ~4008-5446) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("CEF and NSApplication Setup")) {
    // Find the end - before CEFWebViewImpl @interface
    let j = i + 1;
    while (j < lines.length) {
      const t = lines[j].trim();
      if (t.includes("@interface CEFWebViewImpl")) break;
      if (t.includes("RemoteDevToolsClosed(") && lines[j].includes("static_cast<ElectrobunClient*>")) {
        // This is the last CEF function before CEFWebViewImpl
      }
      j++;
    }
    commentRange(i, j - 1);
    break;
  }
}

// ===== CEFWebViewImpl @interface (lines ~5448-5474) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("@interface CEFWebViewImpl : AbstractView")) {
    let j = i;
    while (j < lines.length && !lines[j].trim().startsWith("@end")) j++;
    if (j < lines.length) j++;
    commentRange(i, j - 1);
    break;
  }
}

// ===== initializeCEF() (lines ~5476-5591) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].trim() === "bool initializeCEF() {") {
    let j = i;
    while (j < lines.length && lines[j].trim() !== "}") j++;
    // need to close two braces: the function + inner block
    // count braces to find the real end
    let depth = 0;
    let found = false;
    for (let k = i; k < lines.length; k++) {
      for (const ch of lines[k]) {
        if (ch === '{') depth++;
        if (ch === '}') depth--;
      }
      if (depth === 0 && k > i) {
        commentRange(i, k);
        found = true;
        break;
      }
    }
    if (!found) {
      // fallback: go to blank line before next section
      let k = i;
      while (k < lines.length && lines[k].trim() !== "") k++;
      commentRange(i, k);
    }
    break;
  }
}

// ===== ElectrobunSchemeHandler + Factory + CreateRequestContext (lines ~5594-5826) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].trim() === "// The main scheme handler class") {
    // Go to blank line before CEFWebViewImpl @implementation
    let j = i;
    while (j < lines.length) {
      const t = lines[j].trim();
      if (t.includes("// -------") && t.includes("CEFWebViewImpl")) break;
      j++;
    }
    // Include blank line before CEFWebViewImpl
    while (j > i && lines[j-1].trim() === "") j--;
    commentRange(i, j);
    break;
  }
}

// ===== wgpuCreateAdapterDeviceMainThread =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("extern \"C\" void wgpuCreateAdapterDeviceMainThread")) {
    let depth = 0;
    for (let j = i; j < lines.length; j++) {
      for (const ch of lines[j]) {
        if (ch === '{') depth++;
        if (ch === '}') depth--;
      }
      if (depth === 0 && j > i) {
        commentRange(i, j);
        break;
      }
    }
    break;
  }
}

// ===== CEFWebViewImpl @implementation (lines ~5828-6257) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("@implementation CEFWebViewImpl")) {
    let j = i;
    while (j < lines.length && !lines[j].trim().startsWith("@end")) j++;
    if (j < lines.length) j++;
    commentRange(i, j - 1);
    break;
  }
}

// ===== WGPU input view as first responder (lines ~6351-6359) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("// Prefer WGPU input view as first responder")) {
    // Comment until we hit the closing brace at the same indentation level
    let j = i;
    let braceDepth = 0;
    let started = false;
    while (j < lines.length) {
      for (const ch of lines[j]) {
        if (ch === '{') { started = true; braceDepth++; }
        if (ch === '}') { braceDepth--; }
      }
      if (started && braceDepth <= 0 && lines[j].trim().startsWith("}")) break;
      j++;
    }
    commentRange(i, j);
    break;
  }
}

// ===== useCEF = isCEFAvailable() (line ~6390) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("useCEF = isCEFAvailable()")) {
    lines[i] = "    // useCEF = isCEFAvailable();  // CEF removed";
    break;
  }
}

// ===== if (useCEF) event loop (lines ~6442-6455) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].trim() === "if (useCEF) {" && lines[i+1] && lines[i+1].includes("@autoreleasepool")) {
    // Find the matching closing brace for this if block
    let j = i;
    let depth = 0;
    while (j < lines.length) {
      for (const ch of lines[j]) {
        if (ch === '{') depth++;
        if (ch === '}') depth--;
      }
      if (depth === 0 && j > i) break;
      j++;
    }
    commentRange(i, j);
    break;
  }
}

// ===== if (useCEF) quit (lines ~6474-6480) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].trim() === "if (useCEF) {" && lines[i+2] && lines[i+2].includes("CefQuitMessageLoop")) {
    let j = i;
    let depth = 0;
    while (j < lines.length) {
      for (const ch of lines[j]) {
        if (ch === '{') depth++;
        if (ch === '}') depth--;
      }
      if (depth === 0 && j > i) break;
      j++;
    }
    commentRange(i, j);
    break;
  }
}

// ===== renderer selection (line ~6592) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("Class ImplClass = (strcmp(renderer, \"cef\"") || lines[i].includes("Class ImplClass =")) {
    if (lines[i].includes("CEFWebViewImpl") || lines[i].includes("useCEF")) {
      lines[i] = "    Class ImplClass = [WKWebViewImpl class];  // CEF removed";
      break;
    }
  }
}

// ===== initWGPUView + wgpuView* C exports =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("extern \"C\" AbstractView* initWGPUView")) {
    // Comment until we find addScriptMessageHandlerWithReply (next non-WGPU export)
    let j = i;
    while (j < lines.length) {
      const t = lines[j].trim();
      if (t.startsWith("extern \"C\"") && !t.includes("wgpu") && !t.includes("initWGPU")) break;
      j++;
    }
    commentRange(i, j - 1);
    break;
  }
}

// ===== wgpuViewSetFrame through wgpuViewGetNativeHandle (lines ~6716-6758) =====
for (let i = 0; i < lines.length; i++) {
  if (lines[i].includes("extern \"C\" void wgpuViewSetFrame")) {
    let j = i;
    // Find the next non-wgpu extern "C" function
    while (j < lines.length) {
      const t = lines[j].trim();
      if (t.startsWith("extern \"C\"") && !t.includes("wgpu")) break;
      if (t.startsWith("//") && t.includes("loadURL")) break;
      j++;
    }
    // Go back to find the actual end (before next extern)
    commentRange(i, j - 1);
    break;
  }
}

// Write result
writeFileSync(path, lines.join("\n"), "utf8");
console.log("✓ macOS nativeWrapper.mm patched");

// Count remaining active CEF/WGPU references (not in comments)
let remaining = 0;
const patterns = [/[^\/]\bcef\b/i, /[^\/]\bwgpu\b/i, /[^\/]\bCefRefPtr\b/, /[^\/]\bCefBrowser\b/,
  /[^\/]\bCefClient\b/, /[^\/]\bCefApp\b/, /[^\/]\bCefString\b/, /[^\/]\bCEFWebViewImpl\b/,
  /[^\/]\bWGPUViewImpl\b/, /[^\/]\bWGPUInputView\b/, /[^\/]\binitWGPUView\b/,
  /[^\/]\bwgpuView\b/, /[^\/]\bisCEFAvailable\b/, /[^\/]\bElectrobunClient\b/,
  /[^\/]\bElectrobunHandler\b/, /[^\/]\bElectrobunApp\b/, /[^\/]\bElectrobunSchemeHandler\b/,
  /[^\/]\bElectrobunResponseFilter\b/];
const words = ["cef", "CEF", "wgpu", "WGPU", "CefRefPtr", "CefBrowser", "CefClient", "CefApp",
  "CefString", "CEFWebViewImpl", "WGPUViewImpl", "WGPUInputView", "initWGPUView",
  "wgpuView", "isCEFAvailable", "ElectrobunClient", "ElectrobunApp", "ElectrobunHandler",
  "ElectrobunSchemeHandler", "ElectrobunResponseFilter", "CreateRequestContext",
  "libwebgpu_dawn", "wgpuCreateAdapter", "wgpuInstanceCreateSurface", "wgpuSurfaceConfigure",
  "wgpuSurfaceGetCurrentTexture", "wgpuSurfacePresent", "wgpuQueueOnSubmittedWorkDone",
  "wgpuBufferMapAsync", "wgpuInstanceWaitAny", "wgpuBufferGetMappedRange",
  "wgpuBufferUnmap", "wgpuBufferReadSync", "wgpuBufferReadback"];

for (let i = 0; i < lines.length; i++) {
  const l = lines[i];
  const trimmed = l.trim();
  if (trimmed.startsWith("//") || trimmed.startsWith("/*") || trimmed.startsWith("*")) continue;
  for (const w of words) {
    if (l.includes(w) && !l.trim().startsWith("//")) {
      console.log(`  [${i+1}] ${l.trim().substring(0, 90)}`);
      remaining++;
      break;
    }
  }
}

if (remaining) {
  console.log(`⚠️  ${remaining} active CEF/WGPU references remain (see above)`);
} else {
  console.log("✓ No remaining active CEF/WGPU references found");
}
