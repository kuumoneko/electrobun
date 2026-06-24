import { join, dirname, basename, resolve } from "node:path";
import { OS, ARCH } from "../shared/platform";
import { BUN_VERSION } from "../shared/bun-version";
import {
	getAppFileName,
	getTarballFileName,
} from "../shared/naming";
import { mkdir, rename, rm } from "node:fs/promises";
import downloadAndExtract from "./download";
import { $ } from "bun";

let iconPath = "", tempIcoPath = "";

async function createTar(tarPath: string, cwd: string, distPaths: string[]) {
	await Bun.spawn(["tar", "-cf", `${tarPath}`, ...distPaths], {
		cwd: cwd,
		stdout: "inherit",
		stderr: "inherit",
		stdin: "ignore",
	}).exited
}

// embed icon to exe
async function spawnRcedit(exePath: string, iconPath: string): Promise<void> {
	const rceditPath = join(
		dirname(process.execPath),
		"rcedit-x64.exe",
	);
	if (!(await Bun.file(rceditPath).exists())) {
		throw new Error(
			`rcedit binary not found at ${rceditPath}. ` +
			`Make sure 'rcedit' is installed in your project's node_modules.`,
		);
	}
	if (!(await Bun.file(iconPath).exists())) {
		throw new Error(`Icon path not found at ${iconPath}`)
	}
	await Bun.spawn([`${rceditPath}`, `${exePath}`, "--set-icon", `${iconPath}`], { stdout: "inherit", stderr: "inherit" }).exited;
}

const projectRoot = process.cwd();

const indexOfElectrobun = process.argv.findIndex((arg) =>
	arg.includes("electrobun"),
);
const commandArg = process.argv[indexOfElectrobun + 1] || "build";

async function resolveElectrobunDir(): Promise<string> {
	let dir = projectRoot;
	while (dir !== dirname(dir)) {
		const candidate = join(dir, "node_modules", "electrobun");
		const packageJsonPath = join(candidate, "package.json");
		if (await Bun.file(packageJsonPath).exists()) {
			return candidate;
		}
		dir = dirname(dir);
	}
	return join(projectRoot, "node_modules", "electrobun");
}

const ELECTROBUN_DEP_PATH = await resolveElectrobunDir();
const ELECTROBUN_CACHE_PATH = join(dirname(ELECTROBUN_DEP_PATH), ".electrobun-cache");

function getPlatformPaths(
	targetOS: "macos" | "win" | "linux",
	targetArch: "arm64" | "x64",
) {
	const binExt = targetOS === "win" ? ".exe" : "";
	const platformDistDir = join(
		ELECTROBUN_DEP_PATH,
		`dist-${targetOS}-${targetArch}`,
	);
	const sharedDistDir = join(ELECTROBUN_DEP_PATH, "dist");

	return {
		// Platform-specific binaries (from dist-OS-ARCH/)
		BUN_BINARY: join(platformDistDir, "bun") + binExt,
		LAUNCHER_DEV: join(platformDistDir, "electrobun") + binExt,
		LAUNCHER_RELEASE: join(platformDistDir, "launcher") + binExt,
		NATIVE_WRAPPER_MACOS: join(platformDistDir, "libNativeWrapper.dylib"),
		NATIVE_WRAPPER_WIN: join(platformDistDir, "libNativeWrapper.dll"),
		NATIVE_WRAPPER_LINUX: join(platformDistDir, "libNativeWrapper.so"),
		WEBVIEW2LOADER_WIN: join(platformDistDir, "WebView2Loader.dll"),
		LIBMPV: join(platformDistDir, "libmpv.dll"),
		// MPV: join(platformDistDir, "mpv") + binExt,
		// TEST: join(platformDistDir, "test.dll"),
		SMTC: join(platformDistDir, "smtc.dll"),

		AVFORMAT: join(platformDistDir, "avformat-62.dll"),
		AVCODEC: join(platformDistDir, "avcodec-62.dll"),
		AVUTIL: join(platformDistDir, "avutil-60.dll"),
		LIBSSP: join(platformDistDir, targetOS === "win" ? "libssp-0.dll" : targetOS === "macos" ? "libssp.dylib" : "libssp.so"),
		SWREXAMPLE: join(platformDistDir, "swresample-6.dll"),
		AUMID: join(platformDistDir, "aumid.dll"),
		FILE_DIALOG: join(platformDistDir, "filedialog.dll"),
		BSPATCH: join(platformDistDir, "bspatch") + binExt,
		EXTRACTOR: join(platformDistDir, "extractor") + binExt,
		BSDIFF: join(platformDistDir, "bsdiff") + binExt,
		ZSTD: join(platformDistDir, "zig-zstd") + binExt,
		// These work with existing package.json and development workflow
		MAIN_JS: join(sharedDistDir, "main.js"),
		API_DIR: join(sharedDistDir, "api"),
	};
}

async function ensureCoreDependencies(
	targetOS?: "macos" | "win" | "linux",
	targetArch?: "arm64" | "x64",
) {
	// Use provided target platform or default to host platform
	const platformOS = targetOS || OS;
	const platformArch = targetArch || ARCH;

	// Get platform-specific paths
	const platformPaths = getPlatformPaths(platformOS, platformArch);

	// Check platform-specific binaries
	const requiredBinaries = [
		platformPaths.BUN_BINARY,
		platformPaths.BSDIFF,
		platformPaths.BSPATCH,
		platformPaths.NATIVE_WRAPPER_WIN
	];

	// Check shared files (main.js should be in shared dist/)
	const requiredSharedFiles = [platformPaths.MAIN_JS];

	let missingBinaries: any[] = [];
	let missingSharedFiles: any[] = [];

	await Promise.all([
		async () => {
			for (const filePath of requiredBinaries) {
				if (!(await Bun.file(filePath).exists())) {
					missingBinaries.push(filePath)
				}
			}
		},
		async () => {
			for (const filePath of requiredSharedFiles) {
				if (!(await Bun.file(filePath).exists())) {
					missingSharedFiles.push(filePath)
				}
			}
		},
	])

	if (missingBinaries.length > 0) {
		throw new Error(
			`Binary files missing (expected in production): ${missingBinaries.join(", ")}`
		);
	}

	// If only shared files are missing, that's expected in production (they come via npm)
	if (missingSharedFiles.length > 0) {
		throw new Error(
			`Shared files missing (expected in production): ${missingSharedFiles.map((f) => f.replace(ELECTROBUN_DEP_PATH, ".")).join(", ")}`,
		);
	}
}

async function downloadCustomBun(
	bunVersion: string,
	platformOS: "macos" | "win" | "linux",
	platformArch: "arm64" | "x64",
) {
	const bunUrlSegment: string = "bun-windows-x64.zip";
	const bunDirName: string = "bun-windows-x64";
	const binExt = platformOS === "win" ? ".exe" : "";
	const overrideDir = join(ELECTROBUN_CACHE_PATH, "bun-override", `${platformOS}-${platformArch}`);
	const overrideBinary = join(overrideDir, `bun${binExt}`);
	const bunUrl = `https://github.com/oven-sh/bun/releases/download/bun-v${bunVersion}/${bunUrlSegment}`;

	console.log(`Using custom Bun version: ${bunVersion}`);
	console.log(`Downloading from: ${bunUrl}`);
	await mkdir(overrideDir, { recursive: true });
	await downloadAndExtract(bunUrl, join(overrideDir, "temp.zip"), overrideDir);

	const extractedBinary = join(overrideDir, bunDirName, `bun${binExt}`);
	if (await Bun.file(extractedBinary).exists()) {
		await rename(extractedBinary, overrideBinary);
	} else {
		throw new Error(
			`Bun binary not found after extraction at ${extractedBinary}`,
		);
	}

	if (platformOS !== "win") {
		Bun.spawn(["chmod", "+x", overrideBinary.replace(/"/g, '""')]);
	}

	Bun.file(join(overrideDir, ".bun-version")).write(bunVersion);
}

async function ensureBunBinary(
	targetOS: "macos" | "win" | "linux",
	targetArch: "arm64" | "x64",
	bunVersion?: string,
): Promise<string> {

	const effectiveVersion = bunVersion;
	if (!effectiveVersion) {
		return getPlatformPaths(targetOS, targetArch).BUN_BINARY;
	}

	const binExt = targetOS === "win" ? ".exe" : "";
	const cacheSubdir = "bun-override";
	const overrideDir = join(ELECTROBUN_CACHE_PATH, cacheSubdir, `${targetOS}-${targetArch}`);
	const overrideBinary = join(overrideDir, `bun${binExt}`);
	const versionFile = join(overrideDir, ".bun-version");

	const [isoverrideBinary, isversionFile, isoverrideDir] = [
		await (Bun.file(overrideBinary).exists()),
		await (Bun.file(versionFile).exists()),
		await (Bun.file(overrideDir).exists()),
	]

	// Check if already downloaded with matching version
	if (isoverrideBinary && isversionFile) {
		const cachedVersion = (await Bun.file(versionFile).text()).trim();
		if (cachedVersion === effectiveVersion) {
			console.log(
				`Custom Bun ${effectiveVersion} already cached for ${targetOS}-${targetArch}`,
			);
			return overrideBinary;
		}
		console.log(
			`Cached Bun version "${cachedVersion}" does not match requested "${effectiveVersion}", re-downloading...`,
		);
		await rm(overrideDir, { recursive: true, force: true });
	} else if (isoverrideDir) {
		await rm(overrideDir, { recursive: true, force: true });
	}

	await downloadCustomBun(effectiveVersion, targetOS, targetArch);

	return overrideBinary;
}

// Default values merged with user's electrobun.config.ts
// For the user-facing type, see ElectrobunConfig in src/bun/ElectrobunConfig.ts
const defaultConfig = {
	app: {
		name: "MyApp",
		identifier: "com.example.myapp",
		version: "0.1.0",
		description: "" as string | undefined,
		urlSchemes: undefined as string[] | undefined,
	},
	build: {
		buildFolder: "build",
		artifactFolder: "artifacts",
		useAsar: false,
		asarUnpack: undefined as string[] | undefined, // Glob patterns for files to exclude from ASAR (e.g., ["*.node", "*.dll"])
		bunVersion: undefined as string | undefined, // Override Bun runtime version: "1.4.2"
		bunnyBun: undefined as string | undefined, // Use Electrobunny's Bun fork: "bunny-bun-abc1234" (release tag from blackboardsh/bun)
		locales: undefined as string[] | "*" | undefined, // ICU locales subset (Linux/Windows)
		mac: {
			codesign: false,
			createDmg: true,
			notarize: false,
			bundleWGPU: false,
			entitlements: {
				// This entitlement is required for Electrobun apps with a hardened runtime (required for notarization) to run on macos
				"com.apple.security.cs.allow-jit": true,
				// Required for bun runtime to work with dynamic code execution and JIT compilation when signed
				"com.apple.security.cs.allow-unsigned-executable-memory": true,
				"com.apple.security.cs.disable-library-validation": true,
			} as Record<string, boolean | string>,
			icons: "icon.iconset",
			defaultRenderer: undefined as "native" | undefined,
		},
		win: {
			bundleWGPU: false,
			icon: undefined as string | undefined,
			defaultRenderer: undefined as "native" | undefined,
		},
		linux: {
			bundleWGPU: false,
			icon: undefined as string | undefined,
			defaultRenderer: undefined as "native" | undefined,
		},
		bun: {
			entrypoint: "src/bun/index.ts",
		},
		views: undefined as
			| Record<string, { entrypoint: string;[key: string]: unknown }>
			| undefined,
		copy: undefined as Record<string, string> | undefined,
		watch: undefined as string[] | undefined,
		watchIgnore: undefined as string[] | undefined,
	},
	runtime: {} as Record<string, unknown>,
	scripts: {
		preBuild: "",
		postBuild: "",
		postWrap: "",
		postPackage: "",
	},
	release: {
		baseUrl: "",
		generatePatch: true,
	},
};

function escapeXml(str: string): string {
	return str
		.replace(/&/g, "&amp;")
		.replace(/</g, "&lt;")
		.replace(/>/g, "&gt;")
		.replace(/"/g, "&quot;")
		.replace(/'/g, "&apos;");
}

// Helper function to generate CFBundleURLTypes for custom URL schemes
function generateURLTypes(
	urlSchemes: string[] | undefined,
	identifier: string,
): string {
	if (!urlSchemes || urlSchemes.length === 0) {
		return "";
	}

	const schemesXml = urlSchemes
		.map((scheme) => `                <string>${escapeXml(scheme)}</string>`)
		.join("\n");

	return `    <key>CFBundleURLTypes</key>
    <array>
        <dict>
            <key>CFBundleURLName</key>
            <string>${escapeXml(identifier)}</string>
            <key>CFBundleTypeRole</key>
            <string>Viewer</string>
            <key>CFBundleURLSchemes</key>
            <array>
${schemesXml}
            </array>
        </dict>
    </array>`;
}

const copyIcons = async (IconOutPath: string, icon: string = "") => {
	const iconPath = join(projectRoot, icon ?? "");
	if (await Bun.file(iconPath).exists()) {
		const targetIconPath = join(IconOutPath, "app.ico");
		await Bun.write(targetIconPath, Bun.file(iconPath));
	}
};

(async () => {
	if (commandArg === "build") {
		// Get config
		const configFile = process.argv.find((arg) =>
			arg.startsWith("--config="),
		);

		const config = await getConfig(configFile?.split("--config=")[1] ?? undefined);

		// Get environment
		const envArg =
			process.argv.find((arg) => arg.startsWith("--env="))?.split("=")[1] || "";
		const buildEnvironment = ["dev", "canary", "stable"].includes(envArg)
			? envArg
			: "dev";

		try {
			await runBuild(config, buildEnvironment);
		} catch (error) {
			if (error instanceof Error) {
				console.error(error);
			}
			process.exit(1);
		}
	} else if (commandArg === "run") {
		const config = await getConfig();
		await runAppWithSignalHandling(config);
	} else if (commandArg === "dev") {

		const configFile = process.argv.find((arg) =>
			arg.startsWith("--config="),
		);

		const config = await getConfig(configFile?.split("--config=")[1] ?? undefined);
		try {
			await runBuild(config, "dev");
		} catch (error) {
			if (error instanceof Error) {
				console.error(error.message);
			}
			process.exit(1);
		}

		await runAppWithSignalHandling(config);
	}

	async function runBuild(
		config: Awaited<ReturnType<typeof getConfig>>,
		buildEnvironment: string,
	) {
		const start = new Date().getTime();
		// Determine current platform as default target
		const currentTarget = { os: OS, arch: ARCH };

		// Set up build variables
		const targetOS = currentTarget.os;
		// const targetARCH = currentTarget.arch;
		const targetBinExt = targetOS === "win" ? ".exe" : "";
		const appFileName = getAppFileName(config.app.name, buildEnvironment);

		const platformPrefix = `${buildEnvironment}-${currentTarget.os}-${currentTarget.arch}`

		const buildFolder = join(
			projectRoot,
			config.build.buildFolder,
			platformPrefix,
		);

		const artifactFolder = join(projectRoot, config.build.artifactFolder);

		// Ensure core binaries are available for the target platform before starting build
		await ensureCoreDependencies(currentTarget.os, currentTarget.arch);
		console.log("After ensure Core dependencies:", new Date().getTime() - start);
		// Get platform-specific paths for the current target
		const targetPaths = getPlatformPaths(currentTarget.os, currentTarget.arch);

		// generate icon Path once time
		if (targetOS === "win" && config.build.win?.icon) {
			const iconSourcePath =
				config.build.win.icon.startsWith("/") ||
					config.build.win.icon.match(/^[a-zA-Z]:/)
					? config.build.win.icon
					: join(projectRoot, config.build.win.icon);

			if (await Bun.file(iconSourcePath).exists()) {
				console.log("Found icon source path existed.");
				try {
					if (iconSourcePath.toLocaleLowerCase().endsWith(".ico")) {
						iconPath = iconSourcePath;
					}
					else if (iconSourcePath.toLocaleLowerCase().endsWith(".png")) {
						const pngToIco = (await import("png-to-ico")).default;
						tempIcoPath = join(buildFolder, "temp-icon.ico");
						const icoBuffer = await pngToIco(iconSourcePath);
						await Bun.write(tempIcoPath, Buffer.from(new Uint8Array(icoBuffer)).buffer);
						iconPath = tempIcoPath;
					}
				} catch (error) {
					console.error(error)
				}
			}
		}

		// bundle bun to build/bun
		const bunConfig = config.build.bun;
		const bunSource = join(projectRoot, bunConfig.entrypoint); // file
		if (!(await Bun.file(bunSource).exists())) {
			throw new Error(`Bun source is not existed at ${bunSource}`)
		}

		// build macos bundle
		// Use display name (with spaces) for macOS bundle folders, sanitized name for other platforms
		const bundleName = appFileName;
		const {
			appBundleFolderPath,
			appBundleFolderContentsPath,
			appBundleMacOSPath,
			appBundleFolderResourcesPath,
		} = await createAppBundle(bundleName, buildFolder);

		// app code path
		const appBundleAppCodePath = join(appBundleFolderResourcesPath, "app");
		// CLi launcher binary
		const bunCliLauncherBinarySource = targetPaths.LAUNCHER_RELEASE;
		const bunCliLauncherDestination =
			join(appBundleMacOSPath, "launcher") + targetBinExt;
		const destLauncherFolder = dirname(bunCliLauncherDestination);
		// bun binary
		const bunBinaryDestInBundlePath =
			join(appBundleMacOSPath, "bun") + targetBinExt;
		const destFolder2 = dirname(bunBinaryDestInBundlePath);
		// native wrapper dynamic library
		const nativeWrapperMacosSource = targetPaths.NATIVE_WRAPPER_WIN;
		const nativeWrapperMacosDestination = join(
			appBundleMacOSPath,
			"libNativeWrapper.dll",
		);
		// webview2 library
		const webview2LibSource = targetPaths.WEBVIEW2LOADER_WIN;
		const webview2LibDestination = join(
			appBundleMacOSPath,
			"WebView2Loader.dll",
		);
		// mpv binary
		// const mpvlibSource = targetPaths.MPV;
		//   const mpvlibDestination = join(appBundleMacOSPath, "mpv") + targetBinExt;
		// libmpv binary
		const libmpvSource = targetPaths.LIBMPV;
		const libmpvDestination = join(appBundleMacOSPath, "libmpv.dll");

		// native bindings
		const bsPatchSource = targetPaths.BSPATCH;
		const bsPatchDestination =
			join(appBundleMacOSPath, "bspatch") + targetBinExt;
		const bsPatchDestFolder = dirname(bsPatchDestination);
		// zig-zstd binary
		const zstdSource = targetPaths.ZSTD;
		const zstdDestination = join(appBundleMacOSPath, "zig-zstd") + targetBinExt;

		const avformatSource = targetPaths.AVFORMAT;
		const avformatDestination = join(appBundleMacOSPath, "avformat-62.dll");

		const avcodecSource = targetPaths.AVCODEC;
		const avcodecDestination = join(appBundleMacOSPath, "avcodec-62.dll");

		const avutilSource = targetPaths.AVUTIL;
		const avutilDestination = join(appBundleMacOSPath, "avutil-60.dll");

		const libsspSource = targetPaths.LIBSSP;
		const libsspDestination = join(appBundleMacOSPath, targetOS === "win" ? "libssp-0.dll" : targetOS === "macos" ? "libssp.dylib" : "libssp.so");

		const swresampleSource = targetPaths.SWREXAMPLE;
		const swresampleDestination = join(appBundleMacOSPath, "swresample-6.dll");

		const smtcSource = targetPaths.SMTC;
		const smtcDestination = join(appBundleMacOSPath, "smtc.dll");

		const aumidSource = targetPaths.AUMID;
		const aumidDestination = join(appBundleMacOSPath, "aumid.dll");

		const fileDialogSource = targetPaths.FILE_DIALOG;
		const fileDialogDestination = join(appBundleMacOSPath, "file_dialog.dll");

		// Generate URL scheme handlers
		const urlTypes = generateURLTypes(
			config.app.urlSchemes,
			config.app.identifier,
		);

		const InfoPlistContents = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>launcher</string>
    <key>CFBundleIdentifier</key>
    <string>${config.app.identifier}</string>
    <key>CFBundleName</key>
    <string>${bundleName}</string>
    <key>CFBundleVersion</key>
    <string>${config.app.version}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>${urlTypes ? "\n" + urlTypes : ""}
</dict>
</plist>`;
		// refreshing folder
		await $`rm -rf ${join(buildFolder, appFileName)} `;
		await Promise.all([
			$`mkdir -p ${appBundleAppCodePath}`,
			$`mkdir -p ${destLauncherFolder}`,
			$`mkdir -p ${destFolder2}`,
			$`mkdir -p ${bsPatchDestFolder}`,
			$`rm -rf ${artifactFolder} && mkdir -p ${artifactFolder}`,
			Bun.write(
				join(appBundleFolderContentsPath, "Info.plist"),
				InfoPlistContents,
			),
		])

		// transpile developer's bun code
		const bunDestFolder = join(appBundleAppCodePath, "bun");
		// Build bun-javascript ts files
		const { entrypoint: _bunEntrypoint, ...bunBuildOptions } = bunConfig;

		await Promise.all([
			new Promise((resolve) => {
				ensureBunBinary(
					currentTarget.os,
					currentTarget.arch,
					config.build.bunVersion,
				)
					.then(bunBinarySourcePath => {
						Bun.write(bunBinaryDestInBundlePath, Bun.file(bunBinarySourcePath))
							.then(resolve)
					})
			}),
			Bun.write(bunCliLauncherDestination, Bun.file(bunCliLauncherBinarySource)),
			Bun.write(join(appBundleFolderResourcesPath, "main.js"), Bun.file(targetPaths.MAIN_JS), { createPath: true }),
			Bun.write(nativeWrapperMacosDestination, Bun.file(nativeWrapperMacosSource)),
			Bun.write(webview2LibDestination, Bun.file(webview2LibSource)),
			// Bun.write(mpvlibDestination, Bun.file(mpvlibSource)),
			Bun.write(libmpvDestination, Bun.file(libmpvSource)),
			Bun.write(bsPatchDestination, Bun.file(bsPatchSource)),
			Bun.write(zstdDestination, Bun.file(zstdSource)),
			// Bun.write(testDestination, Bun.file(testSource)),
			Bun.write(smtcDestination, Bun.file(smtcSource)),
			Bun.write(fileDialogDestination, Bun.file(fileDialogSource)),
			Bun.write(aumidDestination, Bun.file(aumidSource)),
			Bun.write(avformatDestination, Bun.file(avformatSource)),
			Bun.write(avcodecDestination, Bun.file(avcodecSource)),
			Bun.write(avutilDestination, Bun.file(avutilSource)),
			Bun.write(libsspDestination, Bun.file(libsspSource)),
			Bun.write(swresampleDestination, Bun.file(swresampleSource)),
			// build bun main process
			new Promise((resolve) => {
				Bun.build({
					...bunBuildOptions,
					entrypoints: [bunSource],
					outdir: bunDestFolder,
					minify: buildEnvironment !== "dev",
					target: "bun",
					naming: "index.js"
				})
					.then(buildResult => {
						if (!buildResult.success) {
							console.error("failed to build", bunSource);
							printBuildLogs(buildResult.logs);
						}
						resolve("")
					})
			}),
			// build views
			new Promise(async (resolve) => {
				for (const viewName in config.build.views) {
					const viewConfig = config.build.views[viewName]!;

					const viewSource = join(projectRoot, viewConfig.entrypoint);
					if (!(await Bun.file(viewSource).exists())) {
						console.error(
							`failed to bundle ${viewSource} because it doesn't exist.`,
						);
						continue;
					}

					const viewDestFolder = join(appBundleAppCodePath, "views", viewName);
					await $`mkdir -p ${viewDestFolder}`

					const { entrypoint: _viewEntrypoint, ...viewBuildOptions } = viewConfig;
					const buildResult = await Bun.build({
						...viewBuildOptions,
						entrypoints: [viewSource],
						outdir: viewDestFolder,
						target: "browser",
					});

					if (!buildResult.success) {
						console.error("failed to build", viewSource);
						printBuildLogs(buildResult.logs);
						continue;
					}
				}
				resolve("");
			}),
			new Promise(async (resolve) => {
				await Promise.all(Object.keys(config.build.copy as any).map(relSource => {
					const source = join(projectRoot, relSource);
					const destination = join(appBundleAppCodePath, (config.build.copy as any)[relSource]!);
					const destFolder = dirname(destination);
					$`mkdir -p ${dirname(source)} && mkdir -p ${destFolder} && cp -R ${source} ${destination}`.catch(console.error)
				}))
				resolve("")
			}),
		])

		console.log("After refreshing folder and build bun main, views, copy:", new Date().getTime() - start);

		// Create a content hash for version.json. In non-dev builds this is used
		// by the updater to detect changes. For dev builds we skip it since
		// the updater isn't relevant.
		let hash: string;
		if (buildEnvironment === "dev") {
			hash = "dev";
		} else {
			// Walk the app bundle and create an in-memory tar for hashing
			// (no temp file on disk). This runs after ASAR packing so the
			// hash reflects the final shipped bundle contents.
			console.time("Generate Bundle hash");
			const bundleFiles: Record<string, Blob> = {};
			const bundleBase = basename(appBundleFolderPath);
			const glob = new Bun.Glob("**/*");

			// 2. Use an async loop for better performance
			// scan() handles the recursion and returns relative paths
			for await (const entryPath of glob.scan(appBundleFolderPath)) {
				const fullPath = `${appBundleFolderPath}/${entryPath}`;
				const file = Bun.file(fullPath);

				// 3. Bun.file().exists() is faster than statSync
				// It returns false for directories, filtering them out automatically
				if (await file.exists()) {
					const bundleKey = `${bundleBase}/${entryPath}`;
					bundleFiles[bundleKey] = file;
				}
			}

			// Check if Bun.Archive is available (Bun 1.3.0+)
			if (typeof Bun.Archive !== "undefined") {
				const archiveBytes = await new Bun.Archive(bundleFiles).bytes();
				// Note: wyhash is the default in Bun.hash but that may change in the future
				// so we're being explicit here.
				hash = Bun.hash.wyhash(archiveBytes, 43770n).toString(36);
			} else {
				// Fallback for older Bun versions - use a simple hash of file paths
				console.warn("Bun.Archive not available, using fallback hash method");
				const fileList = Object.keys(bundleFiles).sort().join("\n");
				hash = Bun.hash.wyhash(fileList).toString(36);
			}
			console.timeEnd("Generate Bundle hash");
		}

		// version.json inside the app bundle
		const versionJsonContent = JSON.stringify({
			version: config.app.version,
			hash: hash,
			channel: buildEnvironment,
			baseUrl: config.release.baseUrl,
			name: appFileName,
			identifier: config.app.identifier,
		});
		const buildJsonObj: Record<string, unknown> = {
			defaultRenderer: "native",
			availableRenderers: ["native"],
			runtime: config.runtime ?? {},
			bunVersion: config.build?.bunVersion ?? BUN_VERSION,
		};

		const buildJsonContent = JSON.stringify(buildJsonObj);

		await Promise.all([
			Bun.write(
				join(appBundleFolderResourcesPath, "version.json"),
				versionJsonContent,
			),
			Bun.write(
				join(appBundleFolderResourcesPath, "build.json"),
				buildJsonContent,
			)
		])
		console.log("After hashing new version: ", new Date().getTime() - start);

		const artifactsToUpload: any[] = [];

		// Linux bundle preparation (skip tar creation for dev environment)
		// For Linux, the app bundle is already in the correct directory structure
		// The tar will be created in the common code path below
		// embed icon
		if (targetOS === "win" && config.build.win?.icon) {
			console.log("Embedding icon...");
			await Promise.all([
				copyIcons(appBundleFolderResourcesPath, config.build.win.icon ?? ""),
				spawnRcedit(bunCliLauncherDestination, iconPath).then(() => console.log(`Successfully embedded icon into launcher.exe`)),
				spawnRcedit(bunBinaryDestInBundlePath, iconPath).then(() => console.log(`Successfully embedded icon into bun.exe`)),
				// spawnRcedit(mpvlibDestination, iconPath).then(() => console.log(`Successfully embedded icon into mpv.exe`))
				spawnRcedit(libmpvDestination, iconPath).then(() => console.log(`Successfully embedded icon into libmpv.dll`))
			])
			console.log("After embedding icon:", new Date().getTime() - start);
		}

		if (buildEnvironment !== "dev") {
			const tarPath = join(
				buildFolder,
				`${appFileName}.tar`,
			);

			const rceditPath = join(
				dirname(process.execPath),
				"rcedit-x64.exe",
			);
			if (!(await Bun.file(rceditPath).exists())) {
				throw new Error(
					`rcedit binary not found at ${rceditPath}. ` +
					`Make sure 'rcedit' is installed in your project's node_modules.`,
				);
			}


			await Bun.spawn([rceditPath,
				bunCliLauncherDestination,
				"--set-version-string", "ProductName", config.app.name,
				"--set-version-string", "FileDescription", config.app.name,
				"--set-version-string", "CompanyName", config.app.identifier,
				"--set-version-string", "ApplicationCompany", config.app.identifier,
				"--set-version-string", "InternalFilename", config.app.name,
				"--set-file-version", config.app.version,
				"--set-product-version", config.app.version,
				"--set-version-string", "LegalCopyright", "©2026 Electrobun. All rights reserved."
			]).exited;


			// Tar the app bundle for all platforms
			await createTar(tarPath, buildFolder, [appFileName]);
			console.log(`After create ${appFileName}.tar:`, new Date().getTime() - start);

			// generate bsdiff
			// https://storage.googleapis.com/eggbun-static/electrobun-playground/canary/ElectrobunPlayground-canary.app.tar.zst
			console.log("baseUrl: ", config.release.baseUrl);

			console.log("generating a patch from the previous version...");

			const generatePatch = async () => {
				// Skip patch generation if disabled
				if (config.release.generatePatch === false) {
					console.log(
						"Patch generation disabled (release.generatePatch = false)",
					);
				} else if (!config.release.baseUrl || config.release.baseUrl.trim() === "") {
					console.log("No baseUrl configured, skipping patch generation");
					console.log(
						"To enable patch generation, configure baseUrl in your electrobun.config",
					);
				} else {
					const tarballFileName = getTarballFileName(appFileName, OS);

					const urlToPrevVersion = await fetch(`${config.release.baseUrl.replace(/\/+$/, "")}/latest`, {
						"headers": {
							'User-Agent': 'node-fetch-example',
							'Accept': 'application/json'
						},
						"body": null,
						"method": "GET"
					});

					const json = await urlToPrevVersion.json();
					const latestVersion = json.tag_name;

					const urlToPrevUpdateJson = `${config.release.baseUrl.replace(/\/+$/, "")}/${latestVersion}/${platformPrefix}-update.json`;
					const cacheBuster = Math.random().toString(36).substring(7);

					const updateJsonResponse = await fetch(urlToPrevUpdateJson);
					const tempp = await updateJsonResponse.arrayBuffer();
					const updateJson = JSON.parse(Buffer.from(tempp).toString("utf8"))

					const urlToLatestTarball = `${config.release.baseUrl.replace(/\/+$/, "")}/${latestVersion}/${platformPrefix}-${tarballFileName}`;

					// attempt to get the previous version to create a patch file
					if (updateJsonResponse && updateJsonResponse.ok) {
						const prevHash = updateJson.hash;
						console.log("PREVIOUS HASH", prevHash);

						const prevVersionCompressedTarballPath = join(
							buildFolder,
							`${prevHash}.tar.zst`,
						);
						let canGeneratePatch = true;
						// todo (yoav): should be able to stream and decompress in the same step
						if (!(await Bun.file(prevVersionCompressedTarballPath).exists())) {
							const response = await fetch(urlToLatestTarball + `?${cacheBuster}`);
							if (response && response.ok && response.body) {
								const reader = response.body.getReader();
								const totalBytesHeader = response.headers.get("content-length");
								const totalBytes = totalBytesHeader
									? Number(totalBytesHeader)
									: undefined;
								let downloadedBytes = 0;
								let lastLogTime = Date.now();
								const logIntervalMs = 5_000;

								const writer = Bun.file(prevVersionCompressedTarballPath).writer();

								while (true) {
									const { done, value } = await reader.read();
									if (done) break;
									downloadedBytes += value.length;
									const now = Date.now();
									if (now - lastLogTime >= logIntervalMs) {
										if (totalBytes && Number.isFinite(totalBytes)) {
											const percent = (
												(downloadedBytes / totalBytes) *
												100
											).toFixed(1);
											console.log(
												`Downloading previous version... ${percent}% (${downloadedBytes}/${totalBytes} bytes)`,
											);
										} else {
											console.log(
												`Downloading previous version... ${downloadedBytes} bytes`,
											);
										}
										lastLogTime = now;
									}
									await writer.write(value);
								}
								await writer.flush();
								writer.end();
							}
							else {
								canGeneratePatch = false;
							}
						}

						console.log("decompress prev funn bundle...");
						const prevTarballPath = join(buildFolder, "prev.tar");

						const zstdPath = targetPaths.ZSTD;
						if (!(await Bun.file(zstdPath).exists())) {
							console.log(
								`zig-zstd not found at ${zstdPath}, skipping patch generation`,
							);
							canGeneratePatch = false;
						}

						if (canGeneratePatch && !(await Bun.file(prevTarballPath).exists())) {
							const decompressResult = await Bun.spawn(
								[
									zstdPath,
									"decompress",
									"-i",
									prevVersionCompressedTarballPath,
									"-o",
									prevTarballPath,
								],
								{
									cwd: buildFolder,
									stdout: "inherit",
									stderr: "inherit",
								},
							).exited
							if (decompressResult !== 0) {
								console.log(
									`Failed to decompress previous tarball (exit code ${decompressResult}), skipping patch generation`,
								);
								canGeneratePatch = false;
							}
						}

						if (canGeneratePatch) {
							console.log("diff previous and new tarballs...");
							const bsdiffpath = targetPaths.BSDIFF;
							const patchFilePath = join(buildFolder, `${appFileName}.patch`);
							const result = Bun.spawnSync(
								[
									bsdiffpath,
									prevTarballPath,
									tarPath,
									patchFilePath,
									"--use-zstd",
									"--level", "3",
								],
								{
									cwd: buildFolder,
									stdout: "inherit",
									stderr: "inherit",
								},
							);
							if (!result.success) {
								// Patch generation is non-critical - users will just download full updates instead of delta patches
								console.error("\n" + "=".repeat(80));
								console.log(result.stderr)
								console.error(
									"WARNING: Patch generation failed (exit code " +
									result.exitCode +
									")",
								);
								console.error(
									"Delta updates will not be available for this release.",
								);
								console.error("Users will download the full update instead.");
								console.error("=".repeat(80) + "\n");
							} else {
								artifactsToUpload.push(patchFilePath);
							}

							// Clean up previous tarball now that bsdiff is done
							await Bun.file(prevTarballPath).unlink().catch(console.error)
						} else {
							console.log(
								"Failed to fetch previous tarball, skipping patch generation",
							);
						}
					} else {
						console.log("prevoius version not found at: ", urlToLatestTarball);
						console.log("skipping diff generation");
					}
				}
			}

			let compressedTarPath = `${tarPath}.zst`;
			const createSelfExtractor = async () => {
				const tarball = Bun.file(tarPath);
				console.log("compressing tarball...");
				if (tarball.size > 0) {
					const zstdPath = targetPaths.ZSTD;
					if (!(await Bun.file(zstdPath).exists())) {
						throw new Error(`zig-zstd not found at ${zstdPath}`);
					}
					const compressResult = Bun.spawnSync(
						[
							zstdPath,
							"compress",
							"-i",
							tarPath,
							"-o",
							compressedTarPath,
							"--threads",
							"max", "-l", "3"
						],
						{
							cwd: buildFolder,
							stdout: "inherit",
							stderr: "inherit",
						},
					);
					if (!compressResult.success) {
						throw new Error(
							`zig-zstd compress failed with exit code ${compressResult.exitCode}`,
						);
					}
				}
				async function buildSelfExtractor() {
					const zigArgs =
						OS === "win"
							? ["-Dtarget=x86_64-windows", "-Dcpu=native"]
							: ARCH === "x64"
								? ["-Dcpu=native"]
								: [];
					const extractorPath = "electrobun/package/src/extractor";
					if (buildEnvironment === "dev") {
						await $`zig build ${zigArgs}`.cwd(extractorPath);
					} else if (buildEnvironment === "stable") {
						await $`zig build -Doptimize=ReleaseFast ${zigArgs}`.cwd(extractorPath)
					}
				}
				console.info("Building Extractor...")
				const now = new Date().getTime();
				// console.log(config.build.win.icon)
				// await Bun.write(resolve("electrobun/package/src/extractor", "app.rc"), `id ICON "favicon.ico"`);
				await copyIcons(resolve("electrobun/package/src/extractor"), config.build.win.icon ?? "");
				await buildSelfExtractor()
				console.log(`Building: ${new Date().getTime() - now} ms`);





				const payloadPath = compressedTarPath;
				const exePath = resolve(`electrobun/package/src/extractor/zig-out/bin/extractor${OS === "win" ? ".exe" : ""}`);
				const outputPath = join(buildFolder, appFileName + "-Setup.exe");
				try {
					// Read the EXE and the Payload into memory
					let exeBuffer: any, payloadBuffer: any;
					await Promise.all([
						Bun.file(exePath).bytes().then(data => exeBuffer = data),
						Bun.file(payloadPath).bytes().then(data => payloadBuffer = data),
					])
					console.log(`   - EXE size: ${(exeBuffer.length / 1024 / 1024).toFixed(2)} MB`);
					console.log(`   - Payload size: ${(payloadBuffer.length / 1024 / 1024).toFixed(2)} MB`);

					// 3. Create the 8-byte size footer
					const footerBuffer = Buffer.alloc(8);
					footerBuffer.writeBigUInt64LE(BigInt(payloadBuffer.length));

					// 4. Combine them all together
					console.log("3. Merging files...");
					const finalBuffer = Buffer.concat([exeBuffer, payloadBuffer, footerBuffer]);
					await $`rm -f ${outputPath}`.catch(console.error);
					// 5. Write the final setup.exe
					await Bun.write(outputPath, new Uint8Array(finalBuffer)).catch(console.error);

					const outfile = Bun.file(outputPath);
					console.log(outfile.size);
					// console.log("sleeping")
					// await Bun.sleep(50000);
					// console.log("done sleeping")
					console.log(`✅ Success! Generated ${outputPath} (${((await Bun.file(outputPath).bytes()).length / 1024 / 1024).toFixed(2)} MB)`);


				} catch (err) {
					console.error("❌ Failed to build installer:", err);
				}
			}

			await generatePatch();
			console.log("After diff patch:", new Date().getTime() - start);
			await createSelfExtractor();
			console.log("After build Self extractor:", new Date().getTime() - start);

			// Write metadata.json to outer bundle (consistent with Windows/Linux)
			const extractorMetadata = {
				identifier: config.app.identifier,
				name: config.app.name,
				channel: buildEnvironment,
				hash: hash,
			};

			let selfExtractingExePath = join(buildFolder, `${appFileName}-Setup.exe`);
			artifactsToUpload.push(selfExtractingExePath);

			await Promise.all([
				Bun.write(
					join(
						appBundleFolderResourcesPath,
						"metadata.json",
					),
					JSON.stringify(extractorMetadata, null, 2),
				),
				Bun.write(join(buildFolder, appFileName + ".metadata.json"), JSON.stringify(extractorMetadata, null, 2)),
			]).catch(console.error)
			artifactsToUpload.push(compressedTarPath)
			console.log("After writting metadata:", new Date().getTime() - start);



			console.log("creating update.json...");
			// update.json for the channel in that channel's build folder
			const updateJsonContent = JSON.stringify({
				version: config.app.version,
				hash: hash.toString(),
				platform: OS,
				arch: ARCH,
			});

			// update.json with platform prefix for flat naming structure
			console.log("moving artifacts...");

			await Promise.all([
				Bun.write(
					join(artifactFolder, `${platformPrefix}-update.json`),
					updateJsonContent,
				),
				new Promise(async (resolve) => {
					for (const artifact of artifactsToUpload) {
						const destination = join(artifactFolder, `${platformPrefix}-${basename(artifact)} `);
						await $`cp -R ${artifact} ${destination} `.catch(console.error)
					}
					resolve("");
				})
			])
			console.log("After moving artifacts:", new Date().getTime() - start);
		}
	}

	// Take over as the terminal's foreground process group (macOS/Linux).
	// This prevents the parent bun script runner from receiving SIGINT
	// when Ctrl+C is pressed, keeping the terminal busy until the app
	// finishes shutting down gracefully.
	// Call once per CLI session — returns a restore function.
	async function takeoverForeground(): Promise<() => void> {
		let restoreFn = () => { };
		if (OS === "win") return restoreFn;
		try {
			const { dlopen, ptr } = await import("bun:ffi");
			const libName = OS === "macos" ? "libSystem.B.dylib" : "libc.so.6";
			const libc = dlopen(libName, {
				open: { args: ["ptr", "i32"], returns: "i32" },
				close: { args: ["i32"], returns: "i32" },
				getpid: { args: [], returns: "i32" },
				setpgid: { args: ["i32", "i32"], returns: "i32" },
				tcgetpgrp: { args: ["i32"], returns: "i32" },
				tcsetpgrp: { args: ["i32", "i32"], returns: "i32" },
				signal: { args: ["i32", "ptr"], returns: "ptr" },
			});

			const ttyPathBuf = new Uint8Array(Buffer.from("/dev/tty\0"));
			const ttyFd = libc.symbols.open(ptr(ttyPathBuf), 2); // O_RDWR

			if (ttyFd >= 0) {
				const originalPgid = libc.symbols.tcgetpgrp(ttyFd);
				if (originalPgid >= 0) {
					// Ignore SIGTTOU at C level so tcsetpgrp works from background group.
					// bun's process.on("SIGTTOU") doesn't set the C-level disposition.
					// SIG_IGN = (void(*)(int))1, SIGTTOU = 22 on macOS/Linux
					libc.symbols.signal(22, 1);

					if (libc.symbols.setpgid(0, 0) === 0) {
						const myPid = libc.symbols.getpid();
						if (libc.symbols.tcsetpgrp(ttyFd, myPid) === 0) {
							restoreFn = () => {
								try {
									libc.symbols.signal(22, 1);
									libc.symbols.tcsetpgrp(ttyFd, originalPgid);
									libc.symbols.close(ttyFd);
								} catch { }
							};
						} else {
							libc.symbols.setpgid(0, originalPgid);
							libc.symbols.close(ttyFd);
						}
					} else {
						libc.symbols.close(ttyFd);
					}
				} else {
					libc.symbols.close(ttyFd);
				}
			}
		} catch {
			// Fall back to default behavior (prompt may return early on Ctrl+C)
		}
		return restoreFn;
	}

	async function runApp(
		config: Awaited<ReturnType<typeof getConfig>>,
		options?: { onExit?: () => void },
	): Promise<{ kill: () => void; exited: Promise<number> }> {
		// Launch the already-built dev bundle

		const buildEnvironment = "dev";
		const bundleFileName = getAppFileName(config.app.name, buildEnvironment);
		const buildSubFolder = `${buildEnvironment}-${OS}-${ARCH}`;
		const buildFolder = join(
			projectRoot,
			config.build.buildFolder,
			buildSubFolder,
		);
		// const bundleFileName = appFileName;

		// eslint-disable-next-line @typescript-eslint/no-explicit-any
		let mainProc: any;
		let bundleExecPath: string;
		// @ts-ignore
		let _bundleResourcesPath: string;
		if (OS === "macos") {
			bundleExecPath = join(buildFolder, bundleFileName, "Contents", "MacOS");
			_bundleResourcesPath = join(
				buildFolder,
				bundleFileName,
				"Contents",
				"Resources",
			);
		} else if (OS === "linux") {
			bundleExecPath = join(buildFolder, bundleFileName, "bin");
			_bundleResourcesPath = join(buildFolder, bundleFileName, "Resources");
		} else if (OS === "win") {
			bundleExecPath = join(buildFolder, bundleFileName, "bin");
			_bundleResourcesPath = join(buildFolder, bundleFileName, "Resources");
		} else {
			throw new Error(`Unsupported OS: ${OS} `);
		}

		if (OS === "win") {
			mainProc = Bun.spawn([join(bundleExecPath, "launcher.exe")], {
				stdio: ["inherit", "inherit", "inherit"],
				cwd: bundleExecPath,
			});
		}

		if (!mainProc) {
			throw new Error("Failed to spawn app process");
		}

		const exitedPromise = mainProc.exited.then((code: number) => {
			options?.onExit?.();
			return code ?? 0;
		});

		return {
			kill: () => {
				try {
					mainProc.kill();
				} catch { }
			},
			exited: exitedPromise,
		};
	}

	async function runAppWithSignalHandling(
		config: Awaited<ReturnType<typeof getConfig>>,
	) {
		const restoreForeground = await takeoverForeground();
		const handle = await runApp(config);

		let sigintCount = 0;
		process.on("SIGINT", () => {
			sigintCount++;
			if (sigintCount === 1) {
				console.log(
					"\n[electrobun dev] Shutting down gracefully... (press Ctrl+C again to force quit)",
				);
			} else {
				console.log("\n[electrobun dev] Force quitting...");
				try {
					process.kill(0, "SIGKILL");
				} catch { }
				process.exit(0);
			}
		});

		const code = await handle.exited;
		restoreForeground();
		process.exit(code);
	}

	// Helper functions
	function formatBuildLogEntry(entry: any): string {
		if (!entry || typeof entry !== "object") return String(entry);
		const level = entry.level || "error";
		let message = entry.message || entry.text || String(entry);
		if (entry.location) {
			const loc = entry.location;
			const file = loc.file || loc.path || "unknown";
			const line = loc.line ?? loc.lineText ?? loc.lineNumber ?? "?";
			const col = loc.column ?? loc.col ?? loc.columnNumber ?? "?";
			message += ` (${file}: ${line}: ${col})`;
		}
		return `[bun.build:${level}] ${message} `;
	}

	function printBuildLogs(logs: any[] | undefined | null) {
		if (!logs || logs.length === 0) {
			console.error("[bun.build] No logs returned from Bun.build");
			return;
		}
		for (const entry of logs) {
			console.error(formatBuildLogEntry(entry));
			if (entry?.notes?.length) {
				for (const note of entry.notes) {
					console.error(`  note: ${note.text ?? String(note)} `);
				}
			}
		}
	}

	async function getConfig(customConfig: string = "electrobun.config.ts") {
		let loadedConfig: Partial<typeof defaultConfig> & Record<string, unknown> =
			{};
		const customConfigFile = join(projectRoot, customConfig);
		const defaultConfigFile = join(projectRoot, "electrobun.config.ts");
		const foundConfigPath = await Bun.file(customConfigFile).exists()
			? customConfigFile
			: (await Bun.file(defaultConfigFile).exists())
				? defaultConfigFile
				: null;

		if (foundConfigPath) {
			console.log(`Using config file: ${basename(foundConfigPath)} `);

			try {
				const configModule = await import(foundConfigPath);
				loadedConfig = configModule.default || configModule;

				if (!loadedConfig || typeof loadedConfig !== "object") {
					console.error("Config file must export a default object");
					console.error("using default config instead");
					loadedConfig = {};
				}
			} catch (error) {
				console.error("Failed to load config file:", error);
				console.error("using default config instead");
			}
		}

		// todo (yoav): write a deep clone fn
		return {
			...defaultConfig,
			...loadedConfig,
			app: {
				...defaultConfig.app,
				...(loadedConfig?.app || {}),
			},
			build: {
				...defaultConfig.build,
				...(loadedConfig?.build || {}),
				mac: {
					...defaultConfig.build.mac,
					...(loadedConfig?.build?.mac || {}),
					entitlements: {
						...defaultConfig.build.mac.entitlements,
						...(loadedConfig?.build?.mac?.entitlements || {}),
					},
				},
				win: {
					...defaultConfig.build.win,
					...(loadedConfig?.build?.win || {}),
				},
				linux: {
					...defaultConfig.build.linux,
					...(loadedConfig?.build?.linux || {}),
				},
				bun: {
					...defaultConfig.build.bun,
					...(loadedConfig?.build?.bun || {}),
				},
			},
			runtime: {
				...defaultConfig.runtime,
				...((loadedConfig as Record<string, any>)?.["runtime"] || {}),
			},
			scripts: {
				...defaultConfig.scripts,
				...(loadedConfig?.scripts || {}),
			},
			release: {
				...defaultConfig.release,
				...(loadedConfig?.release || {}),
			},
		};
	}

	// Note: supposedly the app bundle name is relevant to code sign/notarization so we need to make the app bundle and the self-extracting wrapper app bundle
	// have the same name but different subfolders in our build directory. or I guess delete the first one after tar/compression and then create the other one.
	// either way you can pass in the parent folder here for that flexibility.
	// for intel/arm builds on mac we'll probably have separate subfolders as well and build them in parallel.
	async function createAppBundle(
		bundleName: string,
		parentFolder: string,
	) {
		const appBundleFolderPath = join(parentFolder, bundleName);
		const appBundleFolderContentsPath = appBundleFolderPath; // No Contents folder needed
		const appBundleMacOSPath = join(appBundleFolderPath, "bin"); // Use bin instead of MacOS
		const appBundleFolderResourcesPath = join(
			appBundleFolderPath,
			"Resources",
		);
		const appBundleFolderFrameworksPath = join(appBundleFolderPath, "lib"); // Use lib instead of Frameworks

		// Create directories
		await Promise.all([
			$`mkdir -p ${appBundleMacOSPath} `,
			$`mkdir -p ${appBundleFolderResourcesPath} `,
			$`mkdir -p ${appBundleFolderFrameworksPath} `,
		])

		return {
			appBundleFolderPath,
			appBundleFolderContentsPath,
			appBundleMacOSPath,
			appBundleFolderResourcesPath,
			appBundleFolderFrameworksPath,
		};
	}
})().catch((error) => {
	console.error("Fatal error:", error);
	process.exit(1);
});
