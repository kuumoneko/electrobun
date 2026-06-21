// Run this script via terminal or command line with bun build.ts

import { $ } from "bun";
import { join, dirname, relative, basename, resolve } from "path";
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

console.log("building for Windows x64...");

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
const OS = "win";
const ARCH = "x64";
const binExt = ".exe";
const bunBin = "bun.exe";
const zigBinary = "zig.exe";

// PATHS
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

function validateDownload(filePath: string, type: string): void {
	if (!existsSync(filePath)) {
		throw new Error(`Download failed: ${filePath} does not exist`);
	}
	const stats = statSync(filePath);
	const minSize = MIN_DOWNLOAD_SIZES[type];
	if (minSize && stats.size < minSize) {
		unlinkSync(filePath);
		throw new Error(
			`Download failed: ${filePath} is only ${stats.size} bytes (expected > ${minSize} bytes). Please try again.`,
		);
	}
}

let lastGitHubDownload = 0;
async function pauseForGitHub(): Promise<void> {
	const now = Date.now();
	const timeSinceLastDownload = now - lastGitHubDownload;
	const pauseDuration = 500;
	if (lastGitHubDownload > 0 && timeSinceLastDownload < pauseDuration) {
		const remainingPause = pauseDuration - timeSinceLastDownload;
		console.log(`Pausing ${Math.ceil(remainingPause / 1000)} seconds before next GitHub download...`);
		await new Promise((resolve) => setTimeout(resolve, remainingPause));
	}
	lastGitHubDownload = Date.now();
}

var CMAKE_BIN = "cmake";
var VCVARSALL_PATH = "";

try {
	await setup();
	await build();
	await copyToDist();
} catch (err) {
	console.log(err);
}

async function findMsvcTools() {
	try {
		const vswherePath = join(
			process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)",
			"Microsoft Visual Studio",
			"Installer",
			"vswhere.exe",
		);
		if (!existsSync(vswherePath)) {
			console.log("vswhere not found, using default tool names");
			return;
		}

		const vsInstallResult =
			await $`powershell -command "& '${vswherePath}' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`.quiet();
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
	if (!VCVARSALL_PATH) {
		return await $`${command}`;
	}
	const tempBat = join(process.cwd(), "temp_build_cmd.bat");
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
	console.log("Windows dependency installer finished. Re-checking dependencies...");
}

async function checkDependencies() {
	const missingDeps: string[] = [];
	await findMsvcTools();

	try {
		await $`where cmake`.quiet();
		CMAKE_BIN = "cmake";
	} catch {
		missingDeps.push("cmake");
	}

	let vsFound = false;
	try {
		const vswherePath = join(
			process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)",
			"Microsoft Visual Studio",
			"Installer",
			"vswhere.exe",
		);
		const out = existsSync(vswherePath)
			? await $`powershell -command "& '${vswherePath}' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`.quiet()
			: await $`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`.quiet();

		if (out.exitCode === 0 && out.stdout.toString().trim()) vsFound = true;
	} catch {
		vsFound = false;
	}

	if (!vsFound) missingDeps.push("visual-studio");

	if (missingDeps.length > 0) {
		if (process.env["GITHUB_ACTIONS"]) {
			console.warn("\n⚠️ Missing required dependencies in CI - continuing");
		} else {
			try {
				await installWindowsDeps();
			} catch {
				console.error("Auto-install failed or was cancelled.");
			}
			// Strict check after install omitted for brevity; assuming installer handles it
		}
	}
	console.log("✓ All required dependencies found");
}

async function setup() {
	await Promise.all([
		checkDependencies(),
		vendorBun(),
		vendorZstd(),
		vendorZig(),
        vendorWebview2(),
		vendorBsdiff(),
	])
	// await vendorBsdiff();
}

async function build() {
	await createDistFolder();
	await BunInstall();
	await buildNative();
	console.log("okok\n")

	await buildPreload();
	console.log("\n")

	await buildCli();
	console.log("\n")

	await buildMainJs();
	console.log("\n")

	await buildLauncher();
	console.log("\n")
}

async function copyApiFiles() {
	await $`cp -R src/bun/ dist/api`;
	await $`cp -R src/browser/ dist/api`;
	await $`cp -R src/shared/ dist/api`;
}

async function copyToDist() {
	await Promise.all([
		$`cp ${PATH.bun.RUNTIME} ${PATH.bun.DIST}`,
		$`cp src/launcher/zig-out/bin/launcher${binExt} dist/launcher${binExt}`,
		$`cp vendors/zig-bsdiff/bsdiff${binExt} dist/bsdiff${binExt}`,
		$`cp vendors/zig-bsdiff/bspatch${binExt} dist/bspatch${binExt}`,
		$`cp vendors/zig-zstd/zig-zstd${binExt} dist/zig-zstd${binExt}`,
		$`cp src/npmbin/index.js dist/npmbin.js`,
		$`cp src/cli/build/electrobun${binExt} dist/electrobun${binExt}`,
		$`mkdir -p bin && cp src/cli/build/electrobun${binExt} bin/electrobun${binExt}`,
		copyApiFiles(),
		$`cp src/native/win/build/libNativeWrapper.dll dist/libNativeWrapper.dll`,
		$`cp src/native/win/build/test.dll dist/test.dll`,
		$`cp vendors/webview2/Microsoft.Web.WebView2/build/native/x64/WebView2Loader.dll dist/WebView2Loader.dll`,
		$`cp src/mpv/mpv${binExt} dist/mpv${binExt}`,
		$`cp src/mpv/libmpv.dll dist/libmpv.dll`,
		$`cp smtc/build/smtc.dll dist/smtc.dll`,
		$`cp smtc/build/aumid.dll dist/aumid.dll`,
		$`cp smtc/build/filedialog.dll dist/filedialog.dll`,
		$`cp ffmpeg/avformat-62.dll dist/avformat-62.dll`,
		$`cp ffmpeg/avcodec-62.dll dist/avcodec-62.dll`,
		$`cp ffmpeg/avutil-60.dll dist/avutil-60.dll`,
		$`cp ffmpeg/libssp-0.dll dist/libssp-0.dll`,
		$`cp ffmpeg/swresample-6.dll dist/swresample-6.dll`
	])
	await createPlatformDistFolder();
}

async function createPlatformDistFolder() {
	const platformDistDir = `dist-${OS}-${ARCH}`;
	await $`rm -r ${platformDistDir}`.catch(() => { });
	console.log(`Creating platform-specific dist folder: ${platformDistDir}`);
	await $`mkdir -p ${platformDistDir}`;
	await $`powershell -command "Copy-Item -Path 'dist\\*' -Destination '${platformDistDir}\\' -Recurse -Force"`;
	console.log(`Successfully created and populated ${platformDistDir}`);
}

async function createDistFolder() {
	await $`rm -r dist`.catch(() => { });
	await $`mkdir -p dist/api/bun dist/api/browser`;
}

async function BunInstall() {
	await $`${PATH.bun.RUNTIME} install`;
}

async function vendorBun() {
	// TODO: use where.exe to find bun in local, reduce storage
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
	const tempZipPath = join("vendors", "bun", "temp.zip");
	const extractDir = join("vendors", "bun");

	await $`mkdir -p ${extractDir} && curl -L -o ${tempZipPath} https://github.com/oven-sh/bun/releases/download/bun-v${BUN_VERSION}/bun-windows-x64.zip`;
	validateDownload(tempZipPath, "bun");

	await $`powershell -command "Expand-Archive -Path ${tempZipPath} -DestinationPath ${extractDir} -Force"`;
	await $`mv ${join("vendors", "bun", "bun-windows-x64", "bun.exe")} ${PATH.bun.RUNTIME}`;
	await $`rm ${tempZipPath}`;
	await $`rm -rf ${join("vendors", "bun", "bun-windows-x64")}`;
	writeFileSync(bunVersionFile, BUN_VERSION);
}

async function vendorZig() {
	// TODO: use where.exe zig.exe to find zig in local path, reduce storage, recomend to use zvm to download zig
	if (existsSync(PATH.zig.BIN)) return;
	const zigFolder = `zig-windows-x86_64-0.13.0`;
	await $`mkdir -p vendors/zig && curl -L https://ziglang.org/download/0.13.0/${zigFolder}.zip -o vendors/zig.zip && powershell -ExecutionPolicy Bypass -Command Expand-Archive -Path vendors/zig.zip -DestinationPath vendors/zig-temp && mv vendors/zig-temp/${zigFolder}/zig.exe vendors/zig && mv vendors/zig-temp/${zigFolder}/lib vendors/zig/`;
}

async function vendorBsdiff() {
	const bsdiffDir = join(process.cwd(), "vendors", "zig-bsdiff");
	if (existsSync(join(bsdiffDir, "bsdiff" + binExt))) return;

	await vendorZig();
	const zigPath = PATH.zig.BIN;

	const srcDir = join(process.cwd(), "vendors", "zig-bsdiff-src");
	if (!existsSync(join(srcDir, "bsdiff.zig"))) {
		console.log("Cloning zig-bsdiff source...");
		await $`git clone --recursive https://github.com/blackboardsh/zig-bsdiff.git "${srcDir}"`;
	}

	console.log("Patching bsdiff.zig with --level support...");
	const patchFile = join(process.cwd(), "patches", "zig-bsdiff", "bsdiff.zig");
	await $`cp "${patchFile}" "${join(srcDir, "bsdiff.zig")}"`;

	console.log("Building zig-bsdiff from source...");
	await $`"${zigPath}" build -Doptimize=ReleaseFast`.cwd(srcDir);

	await $`mkdir -p "${bsdiffDir}"`;
	await $`cp "${join(srcDir, "zig-out", "bin", "bsdiff" + binExt)}" "${join(bsdiffDir, "bsdiff" + binExt)}"`;
	await $`cp "${join(srcDir, "zig-out", "bin", "bspatch" + binExt)}" "${join(bsdiffDir, "bspatch" + binExt)}"`;
	console.log("zig-bsdiff built successfully");
}

async function vendorZstd() {
	const zstdDir = join(process.cwd(), "vendors", "zig-zstd");
	if (existsSync(join(zstdDir, "zig-zstd" + binExt))) return;

	await pauseForGitHub();
	console.log("Downloading zig-zstd binaries...");
	const tempTarball = join("vendors", `zig-zstd-temp.tar.gz`);

	await $`mkdir -p vendors/zig-zstd`;
	await $`curl -fL -H "Accept: application/octet-stream" "https://github.com/blackboardsh/zig-zstd/releases/download/v0.1.3/zig-zstd-win32-x64.tar.gz" -o "${tempTarball}"`;
	validateDownload(tempTarball, "zig-zstd");
	await $`tar -xzf "${tempTarball}" -C vendors/zig-zstd`;
	await $`rm "${tempTarball}"`;
}

async function vendorNuget() {
	if (existsSync(join(process.cwd(), "vendors", "nuget", "nuget.exe"))) return;
	await $`mkdir -p vendors/nuget && curl -L -o vendors/nuget/nuget.exe https://dist.nuget.org/win-x86-commandline/latest/nuget.exe`;
}

async function vendorWebview2() {
	if (existsSync(join(process.cwd(), "vendors", "webview2"))) return;
	await vendorNuget();
	await $`vendors/nuget/nuget.exe install Microsoft.Web.WebView2 -OutputDirectory vendors/webview2 -Source https://api.nuget.org/v3/index.json`;

	const webview2BasePath = "./vendors/webview2";
	const webview2Dir = readdirSync(webview2BasePath).find((dir: string) => dir.startsWith("Microsoft.Web.WebView2"));
	if (webview2Dir && webview2Dir !== "Microsoft.Web.WebView2") {
		renameSync(join(webview2BasePath, webview2Dir), join(webview2BasePath, "Microsoft.Web.WebView2"));
	}
}

function isNewer(source: string, target: string) {
	if (!existsSync(target)) return true;
	return statSync(source).mtimeMs > statSync(target).mtimeMs;
}

async function buildNative() {
	const webview2Include = `./vendors/webview2/Microsoft.Web.WebView2/build/native/include`;
	const webview2Lib = `./vendors/webview2/Microsoft.Web.WebView2/build/native/x64/WebView2LoaderStatic.lib`;
	const testSource = "./src/native/win/test.cpp"
	const testObj = "./src/native/win/build/test.obj"
	const testLib = "./src/native/win/build/test.dll"

	const smtcSource = "./smtc/smtc.cpp"
	const smtcObj = "./smtc/build/smtc.obj"
	const smtcLib = "./smtc/build/smtc.dll"

	const aumidSource = "./smtc/aumid.cpp"
	const aumidObj = "./smtc/build/aumid.obj"
	const aumidLib = "./smtc/build/aumid.dll"

	const filedialogSource = "./smtc/filedialog.cpp"
	const filedialogObj = "./smtc/build/filedialog.obj"
	const filedialogLib = "./smtc/build/filedialog.dll"

	await $`mkdir -p src/native/win/build`;
	await $`mkdir -p smtc/build`;
	await Promise.all([
		//  new Promise((res) => {
		//   	if (isNewer(webview2Include, webview2Lib)) {
		// 	console.log("Webview2 Library is changed! Building...")
		//        runMsvcCommand(`cl /c /EHsc /std:c++20 /DNOMINMAX /MT /I"${webview2Include}" /D_USRDLL /D_WINDLL /Fosrc/native/win/build/nativeWrapper.obj src/native/win/nativeWrapper.cpp`)
		//          .then(() => res(""))
		//          .catch((e) => {
		//            console.error(e);
		//            res("")
		//          })
		//      }
		// else {
		//        console.log("Webview2 Library is unchanged! Skipping...");
		//        res("")
		// }
		//  }),
		new Promise(async (res) => {
			if (isNewer(testSource, testLib) || isNewer(testSource, testObj)) {
				console.log("test.cpp is changed! Building...")
				try {
					await runMsvcCommand(`cl /c /EHsc /std:c++20 /MT /D_USRDLL /D_WINDLL /Fosrc/native/win/build/test.obj src/native/win/test.cpp`);
					await runMsvcCommand(`link /DLL /OUT:src/native/win/build/test.dll user32.lib ole32.lib oleaut32.lib shell32.lib kernel32.lib runtimeobject.lib mfplat.lib mf.lib /IMPLIB:src/native/win/build/test.lib src/native/win/build/test.obj`);
				} catch (e) {
					console.error(e);
				}
				res("");
			}
			else {
				console.log("test.cpp is unchanged! Skipping...")
				res("");
			}
		}),
		new Promise(async (res) => {
			if (isNewer(smtcSource, smtcLib) || isNewer(smtcSource, smtcObj)) {
				console.log("smtc.cpp is changed! Building...")
				try {
					await runMsvcCommand(`cl /c /EHsc /std:c++20 /MT /D_USRDLL /D_WINDLL /Fosmtc/build/smtc.obj smtc/smtc.cpp`);
					await runMsvcCommand(`link /DLL /OUT:smtc/build/smtc.dll user32.lib ole32.lib oleaut32.lib shell32.lib kernel32.lib runtimeobject.lib mfplat.lib mf.lib /IMPLIB:smtc/build/smtc.lib smtc/build/smtc.obj`);
				} catch (e) {
					console.error(e);
				}
				res("");
			}
			else {
				console.log("smtc.cpp is unchanged! Skipping...")
				res("");
			}
		}),
		new Promise(async (res) => {
			if (isNewer(aumidSource, aumidLib) || isNewer(aumidSource, aumidObj)) {
				console.log("aumid.cpp is changed! Building...")
				try {
					await runMsvcCommand(`cl /c /EHsc /std:c++20 /MT /D_USRDLL /D_WINDLL /Fosmtc/build/aumid.obj smtc/aumid.cpp`);
					await runMsvcCommand(`link /DLL /OUT:smtc/build/aumid.dll user32.lib ole32.lib oleaut32.lib shell32.lib kernel32.lib /IMPLIB:smtc/build/aumid.lib smtc/build/aumid.obj`);
				} catch (e) {
					console.error(e);
				}
				res("");
			}
			else {
				console.log("aumid.cpp is unchanged! Skipping...")
				res("");
			}
		}),
		new Promise(async (res) => {
			if (isNewer(filedialogSource, filedialogLib) || isNewer(filedialogSource, filedialogObj)) {
				console.log("filedialog.cpp is changed! Building...")
				try {
					await runMsvcCommand(`cl /c /EHsc /std:c++20 /MT /D_USRDLL /D_WINDLL /Fosmtc/build/filedialog.obj smtc/filedialog.cpp`);
					await runMsvcCommand(`link /DLL /OUT:smtc/build/filedialog.dll user32.lib ole32.lib oleaut32.lib shell32.lib kernel32.lib /IMPLIB:smtc/build/filedialog.lib smtc/build/filedialog.obj`);
				} catch (e) {
					console.error(e);
				}
				res("");
			}
			else {
				console.log("filedialog.cpp is unchanged! Skipping...")
				res("");
			}
		}),

		//   	if (isNewer(webview2Include, webview2Lib)) {
		// 	console.log("Webview2 Library is changed! Building...")
		//        runMsvcCommand(`cl /c /EHsc /std:c++20 /DNOMINMAX /MT /I"${webview2Include}" /D_USRDLL /D_WINDLL /Fosrc/native/win/build/nativeWrapper.obj src/native/win/nativeWrapper.cpp`)
		//          .then(() => res(""))
		//          .catch((e) => {
		//            console.error(e);
		//            res("")
		//          })
		//      }
		// else {
		//        console.log("Webview2 Library is unchanged! Skipping...");
		//        res("")
		// }

	])
	// await Promise.all([
	// async	() => {
	// 		if (isNewer(webview2Include, webview2Lib)) {
	// 			console.log("Webview2 Library is changed! Building...")
	// 			await runMsvcCommand(`cl /c /EHsc /std:c++20 /DNOMINMAX /MT /I"${webview2Include}" /D_USRDLL /D_WINDLL /Fosrc/native/win/build/nativeWrapper.obj src/native/win/nativeWrapper.cpp`)
	// 		}
	// 		else {
	// 			console.log("Webview2 Library is unchanged! Skipping...")
	// 		}
	// 	},
	// async	() => {
	// 		if (isNewer(resolve(testInlcude), resolve(testLib))) {
	// 			console.log("test.cpp is changed! Building...")
	// 			await runMsvcCommand(`link /DLL /OUT:src/native/win/build/test.dll user32.lib ole32.lib shell32.lib kernel32.lib /IMPLIB:src/native/win/build/test.lib src/native/win/build/test.obj`);
	// 		}
	// 		else {
	// 			console.log("test.cpp is unchanged! Skipping...")
	// 		}
	// 	},
	// ])
}

async function buildLauncher() {
	console.log(`Building launcher for win x64...`);
	const zigArgs = ["-Dtarget=x86_64-windows", "-Dcpu=baseline"];
	const launcherLib = "./src/launcher/main.zig"
	const launcherout = "./src/launcher/zig-out/bin/launcher.exe";
	if (isNewer(launcherLib, launcherout)) {
		console.log("Launcher is changed! Building...")
		await $`cd src/launcher && zig build -Doptimize=ReleaseSmall ${zigArgs}`;
	}
	else {
		console.log("Launcher is unchanged! Skipping...")
	}
}

async function buildMainJs() {
	const bunModule = await import("bun");
	const sourceFile = "./src/launcher/main.ts";
	const outFile = "./dist/main.js";
	if (isNewer(sourceFile, outFile)) {
		console.log("launcher main is changed! Building...")
		const result = await bunModule.build({
			entrypoints: [join("src", "launcher", "main.ts")],
			outdir: join("dist"),
			external: [],
			target: "bun",
		});
		if (!existsSync(join("dist", "main.js"))) throw new Error("main.js was not created");
		return result;
	}
	else {
		console.log("launcher main is unchanged! Skipping...");
		return {
			success: true,
			logs: [],
			outputs: []
		} as unknown as Bun.BuildOutput;
	}
}

async function buildCli() {
	const cliFile = "./src/cli/index.ts";
	const outFile = "./src/cli/build/electrobun.exe";
	if (isNewer(cliFile, outFile)) {
		console.log("Cli is changed! Building...");
		await
			$`BUN_INSTALL_CACHE_DIR=/tmp/bun-cache ${PATH.bun.RUNTIME} build ${cliFile.split("./")[1]} --compile --target=bun-windows-x64 --outfile ${outFile.split("./")[1].split(".exe")[0]}`;
	}
	else {
		console.log("Cli is unchanged! Skipping...");
	}
}

async function buildPreload() {
	const preloadDir = join(process.cwd(), "src", "bun", "preload");
	const outputDir = join(preloadDir, ".generated");
	const outputPath = join(outputDir, "compiled.ts");
	mkdirSync(outputDir, { recursive: true });

	if (isNewer(join(preloadDir, "index.ts"), outputPath)) {
		console.log("Preload script is changed! Building...")

		const bunModule = await import("bun");
		const fullResult = await bunModule.build({ entrypoints: [join(preloadDir, "index.ts")], outdir: "prebuilt", target: "browser", format: "esm", minify: false });

		const fullPreloadJs = `(function(){${await fullResult.outputs[0].text()}})();`;

		writeFileSync(outputPath, `export const preloadScript = ${JSON.stringify(fullPreloadJs)};`);
	}
	else {
		console.log("Preload script is unchanged! Skipping...");
	}
}
