import { $ } from "bun";
import { platform, arch } from "os";
import { join } from "path";
import {
	existsSync,
	readdirSync,
	renameSync,
	readFileSync,
	writeFileSync,
	mkdirSync,
	statSync,
	unlinkSync,
} from "fs";
import { parseArgs } from "util";
import process from "process";
import { BUN_VERSION } from "./src/shared/bun-version";

const { values: args } = parseArgs({
	args: Bun.argv,
	options: {
		release: { type: "boolean" },
		ci: { type: "boolean" },
		npm: { type: "boolean" },
	},
	allowPositionals: true,
});

const CHANNEL: "debug" | "release" = args.release ? "release" : "debug";
const IS_NPM_BUILD = args.npm || false;
const OS = getPlatform();
const ARCH = getArch();
const isWindows = platform() === "win32";
const binExt = OS === "win" ? ".exe" : "";
const libExt = OS === "win" ? ".dll" : OS === "macos" ? ".dylib" : ".so";
const bunBin = isWindows ? "bun.exe" : "bun";
const zigBinary = OS === "win" ? "zig.exe" : "zig";

const PATH = {
	bun: {
		RUNTIME: join(process.cwd(), "vendors", "bun", bunBin),
		DIST: join(process.cwd(), "dist", bunBin),
	},
	zig: {
		BIN: join(process.cwd(), "vendors", "zig", zigBinary),
	},
};

const MIN_DOWNLOAD_SIZES: Record<string, number> = {
	bun: 10 * 1024 * 1024,
	"zig-bsdiff": 100 * 1024,
	"zig-zstd": 100 * 1024,
};

var CMAKE_BIN = "cmake";
var VCVARSALL_PATH = "";

// ── Helpers ──────────────────────────────────────────────────────────────────

function getPlatform(): "win" | "linux" | "macos" {
	switch (platform()) {
		case "win32": return "win";
		case "darwin": return "macos";
		case "linux": return "linux";
		default: throw new Error("unsupported platform");
	}
}

function getArch(): "arm64" | "x64" {
	switch (arch()) {
		case "arm64": return "arm64";
		case "x64": return "x64";
		default: throw new Error("unsupported arch");
	}
}

function isNewer(source: string, target: string) {
	if (!existsSync(target)) return true;
	return statSync(source).mtimeMs > statSync(target).mtimeMs;
}

function validateDownload(filePath: string, type: string): void {
	if (!existsSync(filePath)) {
		throw new Error(`Download failed: ${filePath} does not exist`);
	}
	const stats = statSync(filePath);
	const minSize = MIN_DOWNLOAD_SIZES[type];
	if (minSize && stats.size < minSize) {
		unlinkSync(filePath);
		throw new Error(`Download failed: ${filePath} is only ${stats.size} bytes`);
	}
}

let lastGitHubDownload = 0;

async function pauseForGitHub(): Promise<void> {
	const now = Date.now();
	const timeSinceLastDownload = now - lastGitHubDownload;
	const pauseDuration = 500;
	if (lastGitHubDownload > 0 && timeSinceLastDownload < pauseDuration) {
		const remainingPause = pauseDuration - timeSinceLastDownload;
		console.log(`Pausing ${Math.ceil(remainingPause / 1000)}s before next GitHub download...`);
		await new Promise((resolve) => setTimeout(resolve, remainingPause));
	}
	lastGitHubDownload = Date.now();
}

// ── Entry point ──────────────────────────────────────────────────────────────

try {
	if (IS_NPM_BUILD) {
		await buildForNpm();
	} else {
		await setup();
		await build();
		await copyToDist();
	}
} catch (err) {
	console.log(err);
	process.exit(1);
}

// ── Dependency checking ──────────────────────────────────────────────────────

async function vendorCmake() {
	if (OS !== "macos") return;

	const vendoredCmakePath = join(process.cwd(), "vendors", "cmake", "CMake.app", "Contents", "bin", "cmake");

	try {
		await $`which cmake`.quiet();
		console.log("✓ cmake found in system PATH");
		CMAKE_BIN = "cmake";
		return;
	} catch {
		if (existsSync(vendoredCmakePath)) {
			CMAKE_BIN = vendoredCmakePath;
			console.log("✓ Using vendored cmake");
			return;
		}
	}

	console.log("cmake not found, downloading...");
	const cmakeVersion = "3.30.2";
	const cmakeUrl = `https://github.com/Kitware/CMake/releases/download/v${cmakeVersion}/cmake-${cmakeVersion}-macos-universal.tar.gz`;

	await $`mkdir -p vendors`;
	const tempFile = "vendors/cmake_temp.tar.gz";
	await $`curl -L "${cmakeUrl}" -o "${tempFile}"`;
	await $`cd vendors && tar -xzf cmake_temp.tar.gz`;
	await $`rm -f vendors/cmake_temp.tar.gz`;

	const extractedDir = `vendors/cmake-${cmakeVersion}-macos-universal`;
	if (existsSync(extractedDir)) {
		await $`rm -rf vendors/cmake`;
		await $`mv "${extractedDir}" vendors/cmake`;
	}
	CMAKE_BIN = vendoredCmakePath;
	await $`"${CMAKE_BIN}" --version`;
	console.log("✓ cmake vendored successfully");
}

async function findMsvcTools() {
	if (OS !== "win") return;

	try {
		const vswherePath = join(
			process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)",
			"Microsoft Visual Studio", "Installer", "vswhere.exe",
		);
		if (!existsSync(vswherePath)) {
			console.log("vswhere not found, using default tool names");
			return;
		}
		const vsInstallResult = await $`powershell -command "& '${vswherePath}' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`.quiet();
		if (vsInstallResult.exitCode !== 0 || !vsInstallResult.stdout.toString().trim()) {
			console.log("Could not find Visual Studio installation path");
			return;
		}
		const vsInstallPath = vsInstallResult.stdout.toString().trim();
		VCVARSALL_PATH = join(vsInstallPath, "VC", "Auxiliary", "Build", "vcvarsall.bat");
		if (!existsSync(VCVARSALL_PATH)) {
			console.log("vcvarsall.bat not found at expected location");
			VCVARSALL_PATH = "";
			return;
		}
		console.log("✓ Found MSVC tools with vcvarsall.bat");
	} catch {
		console.log("Could not locate MSVC tools, using default tool names");
	}
}

async function runMsvcCommand(command: string) {
	if (!VCVARSALL_PATH) return await $`${command}`;
	const uniqueId = `${Date.now()}_${Math.floor(Math.random() * 10)}`;
	const tempBat = join(process.cwd(), `temp_build_cmd_${uniqueId}.bat`);
	const batContent = `@echo off\ncall "${VCVARSALL_PATH}" x64 >nul\n${command}`;
	writeFileSync(tempBat, batContent);
	try {
		const result = await $`cmd /c "${tempBat}"`;
		await $`rm "${tempBat}"`.catch(() => { });
		return result;
	} catch (error) {
		await $`rm "${tempBat}"`.catch(() => { });
		throw error;
	}
}

async function installWindowsDeps() {
	const scriptPath = join(process.cwd(), "scripts", "install-windows-deps.ps1");
	if (!existsSync(scriptPath)) {
		throw new Error("Windows installer script missing. Please run the installer manually.");
	}
	console.log("Running Windows dependency installer (may require Administrator privileges)...");
	await $`powershell -ExecutionPolicy Bypass -NoProfile -File "${scriptPath}"`;
	console.log("Windows dependency installer finished.");
}

async function checkDependencies() {
	const missingDeps: string[] = [];

	if (OS === "macos") {
		await vendorCmake();
		try { await $`which make`.quiet(); } catch { missingDeps.push("make (install Xcode Command Line Tools: xcode-select --install)"); }
	} else if (OS === "win") {
		await findMsvcTools();
		try { await $`where cmake`.quiet(); CMAKE_BIN = "cmake"; } catch { missingDeps.push("cmake"); }
		let vsFound = false;
		try {
			const vswherePath = join(
				process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)",
				"Microsoft Visual Studio", "Installer", "vswhere.exe",
			);
			const out = existsSync(vswherePath)
				? await $`powershell -command "& '${vswherePath}' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`.quiet()
				: await $`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`.quiet();
			if (out.exitCode === 0 && out.stdout.toString().trim()) vsFound = true;
		} catch { vsFound = false; }
		if (!vsFound) missingDeps.push("visual-studio");

		if (missingDeps.length > 0) {
			if (process.env["GITHUB_ACTIONS"]) {
				console.warn("\n⚠️ Missing required dependencies in CI - continuing");
			} else {
				try { await installWindowsDeps(); } catch { console.error("Auto-install failed or was cancelled."); }
			}
		}
	} else if (OS === "linux") {
		try { await $`which cmake`.quiet(); CMAKE_BIN = "cmake"; } catch { missingDeps.push("cmake"); }
		try { await $`which make`.quiet(); } catch { missingDeps.push("make"); }
		try { await $`which gcc`.quiet(); } catch { missingDeps.push("build-essential"); }
		try { await $`which g++`.quiet(); } catch { missingDeps.push("g++"); }
		try { await $`which pkg-config`.quiet(); } catch { missingDeps.push("pkg-config"); }
	}

	if (missingDeps.length > 0) {
		console.error("\n⚠️ Missing required dependencies:");
		missingDeps.forEach((dep) => console.error(`  • ${dep}`));
		if (OS === "macos") {
			console.error("\n  Install Xcode Command Line Tools:\n    xcode-select --install");
		} else if (OS === "win") {
			console.error("\n  1. Install Visual Studio 2022 with C++ development tools");
			console.error("  2. Install cmake from: https://cmake.org/download/");
		} else if (OS === "linux") {
			console.error("\n    sudo apt update && sudo apt install -y build-essential cmake pkg-config");
		}
		if (process.env["GITHUB_ACTIONS"]) {
			console.warn("\n⚠️ Running in CI - continuing despite missing dependencies");
		} else {
			throw new Error("Missing required dependencies. Please install them and try again.");
		}
	}
	console.log("✓ All required dependencies found");
}

// ── Setup / Vendoring ────────────────────────────────────────────────────────

async function setup() {
	await checkDependencies();
	await vendorBun();
	await vendorBsdiff();
	await vendorZstd();
	await vendorZig();
	if (OS === "win") {
		await vendorWebview2();
	} else {
		await vendorAsar();
		if (OS === "linux") await vendorLinuxDeps();
	}
}

async function vendorBun() {
	const bunDir = join(process.cwd(), "vendors", "bun");
	const bunVersionFile = join(bunDir, ".bun-version");

	if (existsSync(PATH.bun.RUNTIME)) {
		if (existsSync(bunVersionFile)) {
			if (readFileSync(bunVersionFile, "utf-8").trim() !== BUN_VERSION) {
				unlinkSync(PATH.bun.RUNTIME);
			} else return;
		} else {
			mkdirSync(bunDir, { recursive: true });
			writeFileSync(bunVersionFile, BUN_VERSION);
			return;
		}
	}

	await pauseForGitHub();

	let bunUrlSegment: string;
	let bunDirName: string;

	if (OS === "win") {
		bunUrlSegment = "bun-windows-x64-baseline.zip";
		bunDirName = "bun-windows-x64-baseline";
	} else if (OS === "macos") {
		bunUrlSegment = ARCH === "arm64" ? "bun-darwin-aarch64.zip" : "bun-darwin-x64.zip";
		bunDirName = ARCH === "arm64" ? "bun-darwin-aarch64" : "bun-darwin-x64";
	} else {
		bunUrlSegment = ARCH === "arm64" ? "bun-linux-aarch64.zip" : "bun-linux-x64.zip";
		bunDirName = ARCH === "arm64" ? "bun-linux-aarch64" : "bun-linux-x64";
	}

	const tempZipPath = join("vendors", "bun", "temp.zip");
	const extractDir = join("vendors", "bun");

	await $`mkdir -p ${extractDir} && curl -L -o ${tempZipPath} https://github.com/oven-sh/bun/releases/download/bun-v${BUN_VERSION}/${bunUrlSegment}`;
	validateDownload(tempZipPath, "bun");

	if (isWindows) {
		await $`powershell -command "Expand-Archive -Path ${tempZipPath} -DestinationPath ${extractDir} -Force"`;
	} else {
		await $`unzip -o ${tempZipPath} -d ${extractDir}`;
	}

	if (isWindows) {
		await $`mv ${join("vendors", "bun", bunDirName, "bun.exe")} ${PATH.bun.RUNTIME}`;
	} else {
		await $`mv ${join("vendors", "bun", bunDirName, "bun")} ${PATH.bun.RUNTIME}`;
	}

	if (!isWindows) {
		await $`chmod +x ${PATH.bun.RUNTIME}`;
	}

	await $`rm ${tempZipPath}`;
	await $`rm -rf ${join("vendors", "bun", bunDirName)}`;
	writeFileSync(bunVersionFile, BUN_VERSION);
}

async function vendorZig() {
	if (existsSync(PATH.zig.BIN)) return;

	if (OS === "macos") {
		const zigArch = ARCH === "arm64" ? "aarch64" : "x86_64";
		await $`mkdir -p vendors/zig && curl -L https://ziglang.org/download/0.13.0/zig-macos-${zigArch}-0.13.0.tar.xz | tar -xJ --strip-components=1 -C vendors/zig zig-macos-${zigArch}-0.13.0/zig zig-macos-${zigArch}-0.13.0/lib zig-macos-${zigArch}-0.13.0/doc`;
	} else if (OS === "win") {
		const zigFolder = "zig-windows-x86_64-0.13.0";
		await $`mkdir -p vendors/zig && curl -L https://ziglang.org/download/0.13.0/${zigFolder}.zip -o vendors/zig.zip && powershell -ExecutionPolicy Bypass -Command Expand-Archive -Path vendors/zig.zip -DestinationPath vendors/zig-temp && mv vendors/zig-temp/${zigFolder}/zig.exe vendors/zig && mv vendors/zig-temp/${zigFolder}/lib vendors/zig/`;
	} else if (OS === "linux") {
		const zigArch = ARCH === "arm64" ? "aarch64" : "x86_64";
		await $`mkdir -p vendors/zig && curl -L https://ziglang.org/download/0.13.0/zig-linux-${zigArch}-0.13.0.tar.xz | tar -xJ --strip-components=1 -C vendors/zig zig-linux-${zigArch}-0.13.0/zig zig-linux-${zigArch}-0.13.0/lib zig-linux-${zigArch}-0.13.0/doc`;
	}
}

async function vendorBsdiff() {
	const bsdiffDir = join(process.cwd(), "vendors", "zig-bsdiff");
	const bsdiffBinPath = join(bsdiffDir, "bsdiff" + binExt);
	const bspatchBinPath = join(bsdiffDir, "bspatch" + binExt);

	if (existsSync(bsdiffBinPath) && existsSync(bspatchBinPath)) return;

	await pauseForGitHub();
	console.log("Downloading zig-bsdiff binaries...");

	const bsdiffPlatformMap: Record<string, string> = { macos: "darwin", win: "win32", linux: "linux" };
	const bsdiffPlatform = bsdiffPlatformMap[OS];
	const tarballUrl = `https://github.com/blackboardsh/zig-bsdiff/releases/download/v0.1.19/zig-bsdiff-${bsdiffPlatform}-${ARCH}.tar.gz`;
	const tempTarball = join("vendors", "zig-bsdiff-temp.tar.gz");

	await $`mkdir -p ${bsdiffDir}`;
	await $`curl -L "${tarballUrl}" -o "${tempTarball}"`;
	validateDownload(tempTarball, "zig-bsdiff");
	await $`tar -xzf "${tempTarball}" -C ${bsdiffDir}`;
	await $`rm "${tempTarball}"`;

	if (OS !== "win") {
		await $`chmod +x ${bsdiffBinPath} ${bspatchBinPath}`;
	}
	console.log("✓ zig-bsdiff binaries downloaded successfully");
}

async function vendorZstd() {
	const zstdDir = join(process.cwd(), "vendors", "zig-zstd");
	const zstdBinPath = join(zstdDir, "zig-zstd" + binExt);

	if (existsSync(zstdBinPath)) return;

	await pauseForGitHub();
	console.log("Downloading zig-zstd binaries...");

	const zstdPlatformMap: Record<string, string> = { macos: "darwin", win: "win32", linux: "linux" };
	const zstdPlatform = zstdPlatformMap[OS];
	const tarballUrl = `https://github.com/blackboardsh/zig-zstd/releases/download/v0.1.3/zig-zstd-${zstdPlatform}-${ARCH}.tar.gz`;
	const tempTarball = join("vendors", "zig-zstd-temp.tar.gz");

	await $`mkdir -p ${zstdDir}`;
	await $`curl -fL -H "Accept: application/octet-stream" "${tarballUrl}" -o "${tempTarball}"`;
	validateDownload(tempTarball, "zig-zstd");
	await $`tar -xzf "${tempTarball}" -C ${zstdDir}`;
	await $`rm "${tempTarball}"`;

	if (OS !== "win") {
		await $`chmod +x ${zstdBinPath}`;
	}
	console.log("✓ zig-zstd binaries downloaded successfully");
}

async function vendorAsar() {
	if (OS === "win") return;
	const ASAR_VERSION = "0.2.2";
	const asarBaseDir = join(process.cwd(), "vendors", "zig-asar");
	const asarPlatformMap: Record<string, string> = { macos: "darwin", linux: "linux" };
	const asarPlatform = asarPlatformMap[OS];
	const asarDir = asarBaseDir;
	const asarLib = join(asarDir, "libasar" + libExt);

	if (existsSync(asarLib)) return;

	await pauseForGitHub();
	console.log(`Downloading zig-asar binaries for ${asarPlatform}-${ARCH}...`);

	const tarballUrl = `https://github.com/blackboardsh/zig-asar/releases/download/v${ASAR_VERSION}/zig-asar-${asarPlatform}-${ARCH}.tar.gz`;
	const tempTarball = join("vendors", "zig-asar-temp.tar.gz");

	await $`mkdir -p "${asarDir}"`;
	await $`curl -L "${tarballUrl}" -o "${tempTarball}"`;
	await $`tar -xzf "${tempTarball}" -C "${asarDir}"`;
	await $`rm "${tempTarball}"`;

	if (!existsSync(asarLib)) {
		throw new Error(`ASAR library not found after extraction: ${asarLib}`);
	}
	if (OS !== "win") {
		await $`chmod +x ${asarLib}`;
	}
	console.log("✓ zig-asar library downloaded successfully");
}

async function vendorNuget() {
	if (OS !== "win") return;
	if (existsSync(join(process.cwd(), "vendors", "nuget", "nuget.exe"))) return;
	await $`mkdir -p vendors/nuget && curl -L -o vendors/nuget/nuget.exe https://dist.nuget.org/win-x86-commandline/latest/nuget.exe`;
}

async function vendorWebview2() {
	if (OS !== "win") return;
	if (existsSync(join(process.cwd(), "vendors", "webview2"))) return;
	await vendorNuget();
	await $`vendors/nuget/nuget.exe install Microsoft.Web.WebView2 -OutputDirectory vendors/webview2 -Source https://api.nuget.org/v3/index.json`;

	const webview2BasePath = "./vendors/webview2";
	const webview2Dir = readdirSync(webview2BasePath).find((dir: string) => dir.startsWith("Microsoft.Web.WebView2"));
	if (webview2Dir && webview2Dir !== "Microsoft.Web.WebView2") {
		renameSync(join(webview2BasePath, webview2Dir), join(webview2BasePath, "Microsoft.Web.WebView2"));
	}
}

async function vendorLinuxDeps() {
	if (OS !== "linux") return;

	const requiredPackages = [
		"build-essential", "cmake", "pkg-config",
		"libgtk-3-dev", "libwebkit2gtk-4.0-dev",
		"libayatana-appindicator3-dev", "librsvg2-dev",
		"fuse", "libfuse2", "libpango1.0-dev", "libharfbuzz-dev"
	];

	const distroInfo = await $`grep -E '^(ID|ID_LIKE)=' /etc/os-release`.catch(() => null);
	if (!distroInfo || !(String(distroInfo.stdout).includes("debian") || String(distroInfo.stdout).includes("ubuntu"))) {
		console.log("Non-Debian/Ubuntu distro detected - skipping automatic dependency check");
		console.log(`Please ensure packages are installed: ${requiredPackages.join(", ")}`);
		return;
	}

	console.log("Detected Debian/Ubuntu. Checking dependencies...");
	const missingPackages: string[] = [];
	for (const pkg of requiredPackages) {
		const result = await $`dpkg -l | grep ${pkg}`.catch(() => null);
		if (!result || String(result.stdout).trim() === "") {
			missingPackages.push(pkg);
		}
	}
	if (missingPackages.length > 0) {
		console.log(`Missing packages: ${missingPackages.join(", ")}`);
		console.log(`  sudo apt update && sudo apt install -y ${missingPackages.join(" ")}`);
		if (process.env["GITHUB_ACTIONS"]) {
			console.warn("⚠️ Running in CI - continuing despite missing packages");
		} else {
			console.warn("⚠️ Some features may not work - continuing anyway");
		}
	}
	console.log("All required packages are installed");
}

// ── Build ────────────────────────────────────────────────────────────────────

async function build() {
	await createDistFolder();
	await BunInstall();
	await buildNative();
	await buildPreload();
	await buildCli();
	await buildMainJs();
	await buildLauncher();
}

async function buildForNpm() {
	console.log("Building for npm (JS/TS files only)...");
	await createDistFolder();
	await buildMainJs();
	await buildPreload();
	await copyApiFiles();
	console.log("npm build complete! dist/ contains main.js and api/ folder.");
}

async function createDistFolder() {
	await $`rm -r dist`.catch(() => { });
	await $`mkdir -p dist/api/bun dist/api/browser`;
}

async function copyApiFiles() {
	if (OS === "win") {
		await $`cp -R src/bun/ dist/api`;
		await $`cp -R src/browser/ dist/api`;
		await $`cp -R src/shared/ dist/api`;
	} else {
		await $`cp -R src/bun dist/api/`;
		await $`cp -R src/browser dist/api/`;
		await $`cp -R src/shared dist/api/`;
	}
}

async function BunInstall() {
	await $`${PATH.bun.RUNTIME} install`;
}

// ── Build Native ─────────────────────────────────────────────────────────────

async function buildNative() {
	if (OS === "macos") {
		const asarLib = join(process.cwd(), "vendors", "zig-asar", "libasar.dylib");

		await $`mkdir -p src/native/macos/build`;
		const compileFlags = [
			"clang++", "-c", "src/native/macos/nativeWrapper.mm",
			"-o", "src/native/macos/build/nativeWrapper.o",
			"-fobjc-arc", "-fno-objc-msgsend-selector-stubs",
			"-std=c++20",
		];
		await $`${compileFlags}`;

		await $`mkdir -p src/native/build`;
		const linkFlags = [
			"clang++", "-o", "src/native/build/libNativeWrapper.dylib",
			"src/native/macos/build/nativeWrapper.o",
			asarLib,
			"-framework", "Cocoa", "-framework", "WebKit",
			"-framework", "QuartzCore", "-framework", "Metal",
			"-framework", "MetalKit", "-framework", "UserNotifications",
			"-stdlib=libc++", "-shared",
			"-install_name", "@executable_path/libNativeWrapper.dylib",
			"-Wl,-rpath,@executable_path",
		];
		await $`${linkFlags}`;
		console.log("✓ macOS native wrapper built successfully");
	} else if (OS === "win") {
		const webview2Include = "./vendors/webview2/Microsoft.Web.WebView2/build/native/include";
		const webview2Lib = "./vendors/webview2/Microsoft.Web.WebView2/build/native/x64/WebView2LoaderStatic.lib";

		await $`mkdir -p src/native/win/build`;
		await $`mkdir -p smtc/build`;

		await Promise.all([
			buildWindowsNativeWrapper(webview2Include, webview2Lib),
			buildWindowsSmtc(),
			buildWindowsAumid(),
			buildWindowsFiledialog(),
		]);
	} else if (OS === "linux") {
		if (!process.env["GITHUB_ACTIONS"]) {
			try {
				await $`pkg-config --exists webkit2gtk-4.0 gtk+-3.0 ayatana-appindicator3-0.1`;
				console.log("✓ All required packages found via pkg-config");
			} catch {
				console.warn("⚠️ Some packages might be missing (pkg-config check failed), continuing anyway");
			}
		}

		try {
			let pkgConfigCflags = "";
			let pkgConfigLibs = "";
			let hasAppIndicator = false;

			try {
				const cflagsResult = await $`pkg-config --cflags webkit2gtk-4.0 gtk+-3.0 ayatana-appindicator3-0.1`.quiet();
				pkgConfigCflags = cflagsResult.stdout.toString().trim();
				const libsResult = await $`pkg-config --libs webkit2gtk-4.0 gtk+-3.0 ayatana-appindicator3-0.1`.quiet();
				pkgConfigLibs = libsResult.stdout.toString().trim();
				hasAppIndicator = true;
			} catch {
				try {
					const cflagsResult = await $`pkg-config --cflags webkit2gtk-4.0 gtk+-3.0`.quiet();
					pkgConfigCflags = cflagsResult.stdout.toString().trim();
					const libsResult = await $`pkg-config --libs webkit2gtk-4.0 gtk+-3.0`.quiet();
					pkgConfigLibs = libsResult.stdout.toString().trim();
					console.warn("⚠️ Using pkg-config without ayatana-appindicator3-0.1");
				} catch {
					console.warn("⚠️ pkg-config failed, using fallback flags");
					const targetArch = process.arch === "arm64" ? "aarch64" : "x86_64";
					pkgConfigCflags = `-I/usr/include/gtk-3.0 -I/usr/include/webkit2gtk-4.0 -I/usr/include/glib-2.0 -I/usr/lib/${targetArch}-linux-gnu/glib-2.0/include -I/usr/include/pango-1.0 -I/usr/include/cairo -I/usr/include/gdk-pixbuf-2.0 -I/usr/include/atk-1.0`;
					pkgConfigLibs = "-lgtk-3 -lwebkit2gtk-4.0 -lglib-2.0 -lgobject-2.0";
				}
			}

			await $`mkdir -p src/native/linux/build`;

			const cflagsParts = pkgConfigCflags.split(/\s+/).filter((f) => f);
			const compileCmd = [
				"g++", "-c", "-std=c++20", "-fPIC",
				...cflagsParts,
				...(hasAppIndicator ? [] : ["-DNO_APPINDICATOR"]),
				"-o", "src/native/linux/build/nativeWrapper.o",
				"src/native/linux/nativeWrapper.cpp",
			];
			await $`${compileCmd}`;

			await $`mkdir -p src/native/build`;

			const libsParts = pkgConfigLibs.split(/\s+/).filter((f) => f);
			const linkCmd = [
				"g++", "-shared",
				"-o", "src/native/build/libNativeWrapper.so",
				"src/native/linux/build/nativeWrapper.o",
				...libsParts,
				"-ldl", "-lpthread",
			];
			await $`${linkCmd}`;
			console.log("✓ Linux native wrapper built successfully");
		} catch (error) {
			console.error("Linux native build failed:", error);
			throw error;
		}
	}
}

async function buildWindowsNativeWrapper(webview2Include: string, webview2Lib: string) {
	const nativeWrapperSource = "./src/native/win/nativeWrapper.cpp";
	const nativeWrapperLib = "./src/native/win/build/libNativeWrapper.dll";
	const nativeWrapperObj = "./src/native/win/build/nativeWrapper.obj";

	if (isNewer(nativeWrapperSource, nativeWrapperLib) || isNewer(nativeWrapperSource, nativeWrapperObj)) {
		console.log("nativeWrapper.cpp is changed! Building...");
		try {
			await runMsvcCommand(`cl /c /EHsc /std:c++20 /DNOMINMAX /MT /I"${webview2Include}" /D_USRDLL /D_WINDLL /Fosrc/native/win/build/nativeWrapper.obj src/native/win/nativeWrapper.cpp`);
			await runMsvcCommand(`link /DLL /OUT:src/native/win/build/libNativeWrapper.dll user32.lib ole32.lib shell32.lib shlwapi.lib advapi32.lib dcomp.lib d2d1.lib kernel32.lib comctl32.lib "${webview2Lib}" libcmt.lib /IMPLIB:src/native/win/build/libNativeWrapper.lib src/native/win/build/nativeWrapper.obj`);
		} catch (e) { console.error(e); }
	} else {
		console.log("nativeWrapper.cpp is unchanged! Skipping...");
	}
}

async function buildWindowsSmtc() {
	const source = "./smtc/smtc.cpp";
	const lib = "./smtc/build/smtc.dll";
	const obj = "./smtc/build/smtc.obj";
	if (isNewer(source, lib) || isNewer(source, obj)) {
		console.log("smtc.cpp is changed! Building...");
		try {
			await runMsvcCommand(`cl /c /EHsc /std:c++20 /MT /D_USRDLL /D_WINDLL /Fosmtc/build/smtc.obj smtc/smtc.cpp`);
			await runMsvcCommand(`link /DLL /OUT:smtc/build/smtc.dll user32.lib ole32.lib oleaut32.lib shell32.lib kernel32.lib runtimeobject.lib mfplat.lib mf.lib /IMPLIB:smtc/build/smtc.lib smtc/build/smtc.obj`);
		} catch (e) { console.error(e); }
	} else { console.log("smtc.cpp is unchanged! Skipping..."); }
}

async function buildWindowsAumid() {
	const source = "./smtc/aumid.cpp";
	const lib = "./smtc/build/aumid.dll";
	const obj = "./smtc/build/aumid.obj";
	if (isNewer(source, lib) || isNewer(source, obj)) {
		console.log("aumid.cpp is changed! Building...");
		try {
			await runMsvcCommand(`cl /c /EHsc /std:c++20 /MT /D_USRDLL /D_WINDLL /Fosmtc/build/aumid.obj smtc/aumid.cpp`);
			await runMsvcCommand(`link /DLL /OUT:smtc/build/aumid.dll user32.lib ole32.lib oleaut32.lib shell32.lib kernel32.lib /IMPLIB:smtc/build/aumid.lib smtc/build/aumid.obj`);
		} catch (e) { console.error(e); }
	} else { console.log("aumid.cpp is unchanged! Skipping..."); }
}

async function buildWindowsFiledialog() {
	const source = "./smtc/filedialog.cpp";
	const lib = "./smtc/build/filedialog.dll";
	const obj = "./smtc/build/filedialog.obj";
	if (isNewer(source, lib) || isNewer(source, obj)) {
		console.log("filedialog.cpp is changed! Building...");
		try {
			await runMsvcCommand(`cl /c /EHsc /std:c++20 /MT /D_USRDLL /D_WINDLL /Fosmtc/build/filedialog.obj smtc/filedialog.cpp`);
			await runMsvcCommand(`link /DLL /OUT:smtc/build/filedialog.dll user32.lib ole32.lib oleaut32.lib shell32.lib kernel32.lib /IMPLIB:smtc/build/filedialog.lib smtc/build/filedialog.obj`);
		} catch (e) { console.error(e); }
	} else { console.log("filedialog.cpp is unchanged! Skipping..."); }
}

// ── Build Launcher ───────────────────────────────────────────────────────────

async function buildLauncher() {
	console.log(`Building launcher for ${OS} ${ARCH}...`);

	let zigArgs: string[] = [];
	if (OS === "win") {
		zigArgs = ["-Dtarget=x86_64-windows", "-Dcpu=baseline"];
	} else if (OS === "linux") {
		zigArgs = ARCH === "arm64" ? ["-Dtarget=aarch64-linux"] : ["-Dtarget=x86_64-linux"];
	} else if (OS === "macos") {
		zigArgs = ARCH === "arm64" ? ["-Dtarget=aarch64-macos"] : ["-Dtarget=x86_64-macos"];
	}

	const launcherLib = "./src/launcher/main.zig";
	const launcherOut = `./src/launcher/zig-out/bin/launcher${binExt}`;

	if (isNewer(launcherLib, launcherOut)) {
		console.log("Launcher is changed! Building...");
		if (CHANNEL === "debug") {
			await $`cd src/launcher && ../../vendors/zig/zig build ${zigArgs}`;
		} else {
			await $`cd src/launcher && ../../vendors/zig/zig build -Doptimize=ReleaseSmall ${zigArgs}`;
		}
	} else {
		console.log("Launcher is unchanged! Skipping...");
	}
}

// ── Build Main JS ────────────────────────────────────────────────────────────

async function buildMainJs() {
	const bunModule = await import("bun");
	const sourceFile = "./src/launcher/main.ts";
	const outFile = "./dist/main.js";

	if (isNewer(sourceFile, outFile)) {
		console.log("launcher main is changed! Building...");
		const result = await bunModule.build({
			entrypoints: [join("src", "launcher", "main.ts")],
			outdir: join("dist"),
			external: [],
			target: "bun",
		});
		if (!existsSync(join("dist", "main.js"))) throw new Error("main.js was not created");
		return result;
	} else {
		console.log("launcher main is unchanged! Skipping...");
		return { success: true, logs: [], outputs: [] } as unknown as Bun.BuildOutput;
	}
}

// ── Build CLI ────────────────────────────────────────────────────────────────

async function buildCli() {
	const cliFile = "./src/cli/index.ts";
	const outFile = `./src/cli/build/electrobun${binExt}`;

	if (isNewer(cliFile, outFile)) {
		console.log("Cli is changed! Building...");

		const args = ["build", cliFile.replace("./", ""), "--compile"];
		if (isWindows) args.push("--target=bun-windows-x64-baseline");
		args.push("--outfile", outFile.replace("./", "").replace(binExt, ""));
		await $`BUN_INSTALL_CACHE_DIR=/tmp/bun-cache ${PATH.bun.RUNTIME} ${args}`;
	} else {
		console.log("Cli is unchanged! Skipping...");
	}
}

// ── Build Preload ────────────────────────────────────────────────────────────

async function buildPreload() {
	const preloadDir = join(process.cwd(), "src", "bun", "preload");
	const outputDir = join(preloadDir, ".generated");
	const outputPath = join(outputDir, "compiled.ts");
	mkdirSync(outputDir, { recursive: true });

	if (isNewer(join(preloadDir, "index.ts"), outputPath)) {
		console.log("Preload script is changed! Building...");

		const bunModule = await import("bun");
		const fullResult = await bunModule.build({
			entrypoints: [join(preloadDir, "index.ts")],
			outdir: "prebuilt",
			target: "browser",
			format: "esm",
			minify: false,
		});

		const fullPreloadJs = `(function(){${await fullResult.outputs[0].text()}})();`;
		writeFileSync(outputPath, `export const preloadScript = ${JSON.stringify(fullPreloadJs)};`);
	} else {
		console.log("Preload script is unchanged! Skipping...");
	}
}

// ── Copy to Dist ─────────────────────────────────────────────────────────────

async function copyToDist() {
	await Promise.all([
		$`cp ${PATH.bun.RUNTIME} ${PATH.bun.DIST}`,
		$`cp src/launcher/zig-out/bin/launcher${binExt} dist/launcher${binExt}`,
		$`cp vendors/zig-bsdiff/bsdiff${binExt} dist/bsdiff${binExt}`,
		$`cp vendors/zig-bsdiff/bspatch${binExt} dist/bspatch${binExt}`,
		$`cp vendors/zig-zstd/zig-zstd${binExt} dist/zig-zstd${binExt}`,
		$`cp src/npmbin/index.js dist/npmbin.js`,
		$`cp src/cli/build/electrobun${binExt} dist/electrobun${binExt}`,
		$`cp src/mpv/build/libmpv${libExt} dist/libmpv${libExt}`,
		$`mkdir -p bin && cp src/cli/build/electrobun${binExt} bin/electrobun${binExt}`,
		copyApiFiles(),
	]);

	if (OS === "macos") {
		await $`cp src/native/build/libNativeWrapper.dylib dist/libNativeWrapper.dylib`;
		await $`cp vendors/zig-asar/libasar.dylib dist/libasar.dylib`;
	} else if (OS === "win") {
		await $`cp src/native/win/build/libNativeWrapper.dll dist/libNativeWrapper.dll`;
		await $`cp vendors/webview2/Microsoft.Web.WebView2/build/native/x64/WebView2Loader.dll dist/WebView2Loader.dll`;
		await $`cp smtc/build/smtc.dll dist/smtc.dll`;
		await $`cp smtc/build/aumid.dll dist/aumid.dll`;
		await $`cp smtc/build/filedialog.dll dist/filedialog.dll`;
		await $`cp ffmpeg/avformat-62.dll dist/avformat-62.dll`;
		await $`cp ffmpeg/avcodec-62.dll dist/avcodec-62.dll`;
		await $`cp ffmpeg/avutil-60.dll dist/avutil-60.dll`;
		await $`cp ffmpeg/libssp-0.dll dist/libssp-0.dll`;
		await $`cp ffmpeg/swresample-6.dll dist/swresample-6.dll`;
	} else if (OS === "linux") {
		if (existsSync(join(process.cwd(), "src", "native", "build", "libNativeWrapper.so"))) {
			await $`cp src/native/build/libNativeWrapper.so dist/libNativeWrapper.so`;
		}

		const smtcBuildPath = join(process.cwd(), "src", "smtc", "linux", "build");
		if (existsSync(join(smtcBuildPath, "libsmtc.so"))) {
			console.log("Copying native SMTC and FileDialog libraries to dist...");
			await $`cp ${smtcBuildPath}/libsmtc.so dist/libsmtc.so`;
			await $`cp ${smtcBuildPath}/libfiledialog.so dist/libfiledialog.so`;
		} else {
			console.warn("⚠️ Warning: Native SMTC/FileDialog binaries not found in build directory.");
		}

		const stagingPath = join(process.cwd(), "..", "staging");
		if (existsSync(stagingPath)) {
			console.log("Copying staged FFmpeg shared libraries to dist...");
			await $`cp ${stagingPath}/libavutil.so* dist/`;
			await $`cp ${stagingPath}/libavformat.so* dist/`;
			await $`cp ${stagingPath}/libavcodec.so* dist/`;
			await $`cp ${stagingPath}/libswresample.so* dist/`;
		} else {
			console.warn("⚠️ Warning: Staging directory not found. FFmpeg binaries might be missing from bundle.");
		}
	}

	await createPlatformDistFolder();
}

async function createPlatformDistFolder() {
	const platformDistDir = `dist-${OS}-${ARCH}`;
	await $`rm -r ${platformDistDir}`.catch(() => { });
	console.log(`Creating platform-specific dist folder: ${platformDistDir}`);
	await $`mkdir -p ${platformDistDir}`;

	if (OS === "win") {
		await $`powershell -command "Copy-Item -Path 'dist\\*' -Destination '${platformDistDir}\\' -Recurse -Force"`;
	} else {
		await $`cp -r dist/ ${platformDistDir}/`
	}
	console.log(`Successfully created and populated ${platformDistDir}`);
}
